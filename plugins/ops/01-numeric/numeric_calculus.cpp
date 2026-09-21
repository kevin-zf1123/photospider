#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_calculus.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
Result<ValueDescriptor> metadata(bool integral,
                                 const std::vector<OperationMetadata>& inputs) {
  using Answer = Result<ValueDescriptor>;
  const auto mismatch = [](const char* message) {
    return Answer(Status{ErrorCode::TypeMismatch,
                         message,
                         FailureReason::None,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  const auto& source = inputs[0].descriptor;
  if ((source.element_type != ElementType::Float32 &&
       source.element_type != ElementType::Float64) ||
      source.shape.size() != 1 || source.shape[0] < (integral ? 1U : 2U) ||
      source.shape[0] > (UINT64_C(1) << 40))
    return mismatch("calculus requires Float32/64 [N] within 2^40");
  for (unsigned port = 1; port < inputs.size(); ++port)
    if (inputs[port].descriptor.element_type != source.element_type ||
        inputs[port].descriptor.shape != std::vector<std::uint64_t>{1})
      return mismatch("calculus controls require matching dtype and shape [1]");
  return Answer(source);
}
Result<Value> execute_calculus(const OperationInvocation& call, bool integral,
                               SequenceProfile profile) {
  using Answer = Result<Value>;
  try {
    const auto* budget = resource_internal::metadata_budget();
    const std::function<Status(std::uint64_t)> work =
        [&](std::uint64_t amount) {
          if (call.cancellation.cancelled())
            return Status{ErrorCode::Cancelled, {}};
          return budget ? budget->consume({amount}) : Status::success();
        };
    auto status = work(1);
    if (!status.ok())
      return Answer(status);
    const auto& shape = call.prepared->traits().outputs[0].fixed_output_shape;
    const auto count = shape[0];
    const auto type = call.inputs[0].descriptor().element_type;
    const auto width = Value::element_size(type);
    std::vector<std::uint64_t> coordinate(1, 0);
    const auto read = [&](std::size_t port,
                          std::uint64_t index) -> Result<std::uint64_t> {
      auto charged = work(2);
      if (!charged.ok())
        return Result<std::uint64_t>(charged);
      const auto& value = call.inputs[port];
      coordinate[0] = index;
      auto address = value.byte_address(coordinate);
      if (!address.ok())
        return Result<std::uint64_t>(address.status());
      std::uint64_t bits = 0;
      std::memcpy(&bits, value.bytes().data() + address.value(), width);
      return Result<std::uint64_t>(bits);
    };
    std::uint64_t step = 0, initial = 0;
    if (!integral || count > 1) {
      auto bits = read(1, 0);
      if (!bits.ok())
        return Answer(bits.status());
      step = bits.value();
      const auto parts = numeric_ops::BinaryParts::decode(step, width == 4);
      if (!parts.magnitude || parts.nan || parts.infinite)
        return Answer(
            Status{ErrorCode::InvalidArgument,
                   "InvalidSampleStep: port=1 bits=" + std::to_string(step),
                   FailureReason::InvalidDomain,
                   {FailureOrigin::Domain, FailureScope::Run}});
    }
    if (integral) {
      auto bits = read(count == 1 ? 0 : 2, 0);
      if (!bits.ok())
        return Answer(bits.status());
      initial = bits.value();
    }
    auto allocated = MutableValue::allocate({type, shape}, call.output_region,
                                            call.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto output = allocated.take_value();
    const auto store = [&](std::uint64_t index, std::uint64_t bits) {
      std::array<std::uint64_t, 4> replicas{};
      numeric_ops::select_words(replicas.data(), bits, bits, 1, profile);
      std::memcpy(output.data() + index * width, replicas.data(), width);
      return work(1);
    };
    if (integral) {
      status = store(0, initial);
      if (!status.ok())
        return Answer(status);
    }
    if (integral && count == 1)
      return std::move(output).publish();
    auto scratch = call.allocator.allocate(sizeof(numeric_ops::ExactCalculus));
    if (!scratch.ok())
      return Answer(scratch.status());
    auto buffer = scratch.take_value();
    std::unique_ptr<numeric_ops::ExactCalculus,
                    void (*)(numeric_ops::ExactCalculus*)>
        arithmetic(
            new (buffer.data()) numeric_ops::ExactCalculus(profile, type),
            [](numeric_ops::ExactCalculus* value) { value->~ExactCalculus(); });
    for (std::uint64_t index = 0; index < count; ++index) {
      Result<std::uint64_t> calculated(initial);
      if (integral) {
        auto bits = read(0, index);
        if (!bits.ok())
          return Answer(bits.status());
        status = arithmetic->add(bits.value(), work);
        if (!status.ok())
          return Answer(status);
        if (!index)
          continue;
        calculated = arithmetic->integral(step, initial, work);
      } else {
        const auto low = index == 0 ? 0 : index - 1,
                   high = index == count - 1 ? count - 1 : index + 1;
        auto a = read(0, low);
        if (!a.ok())
          return Answer(a.status());
        auto b = read(0, high);
        if (!b.ok())
          return Answer(b.status());
        calculated = arithmetic->derivative(a.value(), b.value(), step,
                                            high - low == 2, work);
      }
      if (!calculated.ok())
        return Answer(calculated.status());
      status = store(index, calculated.value());
      if (!status.ok())
        return Answer(status);
    }
    status = work(1);
    return status.ok() ? std::move(output).publish() : Answer(status);
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted,
                         {},
                         FailureReason::CapacityLimit,
                         {FailureOrigin::Resource, FailureScope::Run}});
  }
}
OperationDefinition calculus_operation(const std::string& key, bool integral,
                                       SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = integral ? 3 : 2;
  traits.input_schema.resize(traits.input_count);
  traits.requires_metadata_specialization = true;
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Whole;
  output.requires_dense_output = true;
  traits.workspace_bytes = sizeof(numeric_ops::ExactCalculus);
  operation.specialize_metadata =
      [integral, profile](
          const auto& inputs,
          const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto resolved = metadata(integral, inputs);
    if (!resolved.ok())
      return Answer(resolved.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = resolved.take_value();
    if (integral && result.metadata.descriptor.shape[0] == 1)
      result.input_indices = std::vector<std::uint32_t>{2};
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.callback = [integral, profile](const OperationInvocation& call) {
    return execute_calculus(call, integral, profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_calculus(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool integral : {false, true}) {
      auto status = registry->register_operation(calculus_operation(
          std::string("numeric.") +
              (integral ? "integrate_1d" : "derivative_1d") + profile.first,
          integral, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
