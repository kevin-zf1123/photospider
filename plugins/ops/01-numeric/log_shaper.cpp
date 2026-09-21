#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_shaper.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
struct ShaperState {
  numeric_ops::ExactShaper arithmetic;
  explicit ShaperState(SequenceProfile profile) : arithmetic(profile) {}
};
Result<Value> execute_shaper(const OperationInvocation& call, bool inverse,
                             SequenceProfile profile) {
  using Answer = Result<Value>;
  try {
    const auto* budget = resource_internal::metadata_budget();
    const std::function<Status(std::uint64_t)> consume = [&](auto amount) {
      if (call.cancellation.cancelled())
        return Status{ErrorCode::Cancelled, {}};
      return budget ? budget->consume({amount}) : Status::success();
    };
    auto read = [&](unsigned port, const std::vector<std::uint64_t>& at,
                    std::uint64_t* bits) {
      auto status = consume(at.size() + 1);
      if (!status.ok())
        return status;
      auto address = call.inputs[port].byte_address(at);
      if (!address.ok())
        return address.status();
      std::memcpy(
          bits, call.inputs[port].bytes().data() + address.value(),
          Value::element_size(call.inputs[port].descriptor().element_type));
      return Status::success();
    };
    const auto descriptor = call.inputs[0].descriptor();
    const bool narrow = descriptor.element_type == ElementType::Float32;
    const unsigned width = narrow ? 4 : 8;
    std::array<std::uint64_t, 2> bounds{};
    std::array<BinaryParts, 2> parts{};
    const auto invalid = [&](unsigned i) {
      return Status{ErrorCode::InvalidArgument,
                    "InvalidBounds: port=" + std::to_string(i + 1) +
                        " bits=" + std::to_string(bounds[i]),
                    FailureReason::InvalidDomain,
                    {FailureOrigin::Domain, FailureScope::Run}};
    };
    for (unsigned i = 0; i < 2; ++i) {
      auto status = read(i + 1, {0}, &bounds[i]);
      if (!status.ok())
        return Answer(status);
      parts[i] = BinaryParts::decode(bounds[i], narrow);
    }
    for (unsigned i = 0; i < 2; ++i)
      if (parts[i].nan || parts[i].infinite || parts[i].negative ||
          !parts[i].magnitude)
        return Answer(invalid(i));
    if (parts[0].order_key() >= parts[1].order_key())
      return Answer(invalid(0));
    auto scratch = call.allocator.allocate(sizeof(ShaperState));
    if (!scratch.ok())
      return Answer(scratch.status());
    auto buffer = scratch.take_value();
    std::unique_ptr<ShaperState, void (*)(ShaperState*)> state(
        new (buffer.data()) ShaperState(profile),
        [](auto* value) { value->~ShaperState(); });
    auto allocated =
        MutableValue::allocate(descriptor, call.output_region, call.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto output = allocated.take_value();
    std::vector<std::uint64_t> at(descriptor.shape.size(), 0);
    const auto count = call.output_region.element_count().value();
    for (std::uint64_t i = 0; i < count; ++i) {
      std::uint64_t input = 0;
      auto status = read(0, at, &input);
      if (!status.ok())
        return Answer(status);
      auto result = state->arithmetic.evaluate(
          input, bounds[0], bounds[1], inverse, narrow, consume,
          [] { return Status::success(); });
      if (!result.ok())
        return Answer(result.status());
      const auto bits = result.value();
      std::memcpy(output.data() + i * width, &bits, width);
      for (auto j = at.size(); j; --j) {
        if (++at[j - 1] < descriptor.shape[j - 1])
          break;
        at[j - 1] = 0;
      }
    }
    auto status = consume(1);
    return status.ok() ? std::move(output).publish() : Answer(status);
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted,
                         {},
                         FailureReason::CapacityLimit,
                         {FailureOrigin::Resource, FailureScope::Run}});
  }
}
OperationDefinition operation(const std::string& key, bool inverse,
                              SequenceProfile profile) {
  OperationDefinition result;
  result.key = key;
  auto& traits = result.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  for (auto& input : traits.input_schema)
    input.element_type_mask = 12;
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::PreserveFirstInput;
  output.output_dtype_rule = OperationDtypeRule::Input;
  output.region_rule = OperationRegionRule::Whole;
  output.requires_dense_output = true;
  traits.workspace_bytes = sizeof(ShaperState);
  traits.requires_metadata_specialization = true;
  result.specialize_metadata = [profile](const auto& inputs, const auto&) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto mismatch = [](const char* message) {
      return Status{ErrorCode::TypeMismatch,
                    message,
                    FailureReason::None,
                    {FailureOrigin::Schema, FailureScope::Unspecified}};
    };
    if (inputs.size() != 3 || inputs[0].descriptor.shape.empty() ||
        inputs[0].descriptor.shape.size() > 8)
      return Answer(mismatch("shaper requires rank-1..8 input and two bounds"));
    const auto type = inputs[0].descriptor.element_type;
    if (type != ElementType::Float32 && type != ElementType::Float64)
      return Answer(mismatch("shaper requires Float32/64"));
    for (unsigned i = 1; i < 3; ++i)
      if (inputs[i].descriptor.element_type != type ||
          inputs[i].descriptor.shape != std::vector<std::uint64_t>{1})
        return Answer(
            mismatch("shaper bounds require matching dtype and shape [1]"));
    std::uint64_t count = 1;
    for (auto extent : inputs[0].descriptor.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Answer(mismatch("shaper logical product exceeds 2^40 values"));
      count *= extent;
    }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    resolved.metadata.descriptor = inputs[0].descriptor;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  result.callback = [inverse, profile](const OperationInvocation& call) {
    return execute_shaper(call, inverse, profile);
  };
  return result;
}
}  // namespace
Status register_log_shapers(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (bool inverse : {false, true}) {
      auto status = registry->register_operation(
          operation(std::string(inverse ? "curve.log2_shaper_inverse"
                                        : "curve.log2_shaper") +
                        profile.first,
                    inverse, profile.second));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
