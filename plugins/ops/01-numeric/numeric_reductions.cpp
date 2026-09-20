#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_moments.hpp"
#include "data/input_validation.hpp"
#include "photospider/data/semantic.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
enum class ReductionKind { Sum, Minimum, Maximum, Mean, Count, Variance, Std };
struct ReductionMetadata {
  unsigned mask = 0;
  std::uint64_t count = 1, ddof = 0;
  ValueDescriptor output;
};
Result<ReductionMetadata> metadata(
    ReductionKind kind, const OperationMetadata& input,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<ReductionMetadata>;
  const auto mismatch = [](const char* message) {
    return Answer(Status{ErrorCode::TypeMismatch,
                         message,
                         FailureReason::None,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  const auto& descriptor = input.descriptor;
  if (descriptor.shape.empty() || descriptor.shape.size() > 8)
    return mismatch("reduction requires rank 1..8");
  std::uint64_t elements = 1;
  for (auto size : descriptor.shape) {
    if (!size || size > (UINT64_C(1) << 40) / elements)
      return mismatch("reduction input exceeds 2^40 elements");
    elements *= size;
  }
  auto axes = numeric_ops::parse_array_list(
      std::get<std::string>(parameters.at("axes")), false);
  if (!axes.ok())
    return Answer(axes.status());
  ReductionMetadata result;
  result.output = descriptor;
  for (auto axis : axes.value()) {
    if (axis >= descriptor.shape.size() || (result.mask & (1U << axis)))
      return Answer(numeric_ops::array_parameter_error(
          "invalid or duplicate reduction axis"));
    result.mask |= 1U << axis;
    result.count *= descriptor.shape[axis];
    result.output.shape[axis] = 1;
  }
  if (kind == ReductionKind::Count)
    result.output.element_type = ElementType::Int64;
  if (kind == ReductionKind::Sum || kind == ReductionKind::Mean ||
      kind == ReductionKind::Variance || kind == ReductionKind::Std) {
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    if (dtype == "uint8")
      result.output.element_type = ElementType::UInt8;
    else if (dtype == "int64")
      result.output.element_type = ElementType::Int64;
    else if (dtype == "float32")
      result.output.element_type = ElementType::Float32;
    else if (dtype == "float64")
      result.output.element_type = ElementType::Float64;
    else
      return Answer(
          numeric_ops::array_parameter_error("unsupported reduction dtype"));
    const bool source_float = descriptor.element_type == ElementType::Float32 ||
                              descriptor.element_type == ElementType::Float64;
    const bool target_float =
        result.output.element_type == ElementType::Float32 ||
        result.output.element_type == ElementType::Float64;
    if (kind == ReductionKind::Sum ? source_float != target_float
                                   : !target_float)
      return mismatch("reduction source/destination numeric domains differ");
  }
  if (kind == ReductionKind::Variance || kind == ReductionKind::Std) {
    const auto ddof = std::get<std::int64_t>(parameters.at("ddof"));
    if (ddof < 0 || static_cast<std::uint64_t>(ddof) >= result.count)
      return Answer(numeric_ops::array_parameter_error(
          "InvalidDegreesOfFreedom: require 0<=ddof<N"));
    result.ddof = static_cast<std::uint64_t>(ddof);
  }
  return Answer(std::move(result));
}
const char* operation_name(ReductionKind kind) {
  switch (kind) {
    case ReductionKind::Sum:
      return "reduce_sum";
    case ReductionKind::Minimum:
      return "reduce_minimum";
    case ReductionKind::Maximum:
      return "reduce_maximum";
    case ReductionKind::Mean:
      return "reduce_mean";
    case ReductionKind::Count:
      return "reduce_count";
    case ReductionKind::Variance:
      return "reduce_variance";
    case ReductionKind::Std:
      return "reduce_std";
  }
  return "invalid";
}
struct ReductionState final {
  std::variant<numeric_ops::ExactAggregate, numeric_ops::ExactMoments>
      arithmetic;
  ReductionState(ReductionKind kind, SequenceProfile profile, ElementType type)
      : arithmetic(std::in_place_type<numeric_ops::ExactAggregate>, profile,
                   kind == ReductionKind::Minimum
                       ? numeric_ops::AggregateKind::Minimum
                   : kind == ReductionKind::Maximum
                       ? numeric_ops::AggregateKind::Maximum
                       : numeric_ops::AggregateKind::Sum,
                   type) {
    if (kind == ReductionKind::Variance || kind == ReductionKind::Std)
      arithmetic.emplace<numeric_ops::ExactMoments>(profile, type);
  }
};
Result<Value> execute_reduction(const OperationInvocation& call,
                                ReductionKind kind, SequenceProfile profile) {
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
    const auto input_metadata =
        call.input_metadata.empty()
            ? OperationMetadata{call.inputs[0].descriptor(),
                                call.inputs[0].facets()}
            : call.input_metadata[0];
    auto parsed = metadata(kind, input_metadata, call.parameters);
    if (!parsed.ok())
      return Answer(parsed.status());
    const auto& description = parsed.value();
    if (kind == ReductionKind::Count) {
      status = work(input_metadata.descriptor.shape.size() + 16);
      if (!status.ok())
        return Answer(status);
      auto allocated = call.allocator.allocate(8);
      if (!allocated.ok())
        return Answer(allocated.status());
      auto bytes = allocated.take_value();
      std::array<std::uint64_t, 4> replicas{};
      numeric_ops::select_words(replicas.data(), description.count,
                                description.count, 1, profile);
      std::memcpy(bytes.data(), replicas.data(), 8);
      status = work(1);
      if (!status.ok())
        return Answer(status);
      return Value::from_storage(
          description.output, call.output_region,
          {0, std::vector<std::int64_t>(description.output.shape.size(), 0)},
          std::move(bytes).freeze());
    }
    auto scratch = call.allocator.allocate(sizeof(ReductionState));
    if (!scratch.ok())
      return Answer(scratch.status());
    auto buffer = scratch.take_value();
    std::unique_ptr<ReductionState, void (*)(ReductionState*)> state(
        new (buffer.data()) ReductionState(
            kind, profile, input_metadata.descriptor.element_type),
        [](ReductionState* item) { item->~ReductionState(); });
    auto allocated = MutableValue::allocate(description.output,
                                            call.output_region, call.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto output = allocated.take_value();
    const auto& input = call.inputs[0];
    const auto& shape = input.descriptor().shape;
    const auto width = Value::element_size(input.descriptor().element_type),
               out_width = Value::element_size(description.output.element_type);
    std::vector<std::uint64_t> coordinate(shape.size(), 0),
        source(shape.size(), 0);
    auto count = call.output_region.element_count();
    if (!count.ok())
      return Answer(count.status());
    for (std::uint64_t group = 0; group < count.value(); ++group) {
      std::visit([](auto& arithmetic) { arithmetic.reset(); },
                 state->arithmetic);
      source = coordinate;
      for (std::uint64_t i = 0; i < description.count; ++i) {
        status = work(shape.size() + 1);
        if (!status.ok())
          return Answer(status);
        auto address = input.byte_address(source);
        if (!address.ok())
          return Answer(address.status());
        std::uint64_t bits = 0;
        std::memcpy(&bits, input.bytes().data() + address.value(), width);
        status = std::visit(
            [&](auto& arithmetic) { return arithmetic.add(bits, work); },
            state->arithmetic);
        if (!status.ok())
          return Answer(status);
        for (std::size_t j = shape.size(); j; --j)
          if (description.mask & (1U << (j - 1))) {
            if (++source[j - 1] < shape[j - 1])
              break;
            source[j - 1] = 0;
          }
      }
      Result<std::uint64_t> result(
          Status{ErrorCode::Internal, "uninitialized reduction"});
      if (auto* moments =
              std::get_if<numeric_ops::ExactMoments>(&state->arithmetic))
        result =
            moments->finish(description.output.element_type, description.count,
                            description.ddof, kind == ReductionKind::Std, work);
      else
        result =
            std::get<numeric_ops::ExactAggregate>(state->arithmetic)
                .finish_as(description.output.element_type,
                           kind == ReductionKind::Mean ? description.count : 1,
                           work);
      if (!result.ok()) {
        auto failure = result.status();
        if (failure.reason == FailureReason::ArithmeticOverflow) {
          failure.detail = {FailureOrigin::Domain, FailureScope::Run};
          failure.message += " output linear=" + std::to_string(group);
        }
        return Answer(failure);
      }
      std::array<std::uint64_t, 4> replicas{};
      numeric_ops::select_words(replicas.data(), result.value(), result.value(),
                                1, profile);
      std::memcpy(output.data() + group * out_width, replicas.data(),
                  out_width);
      for (std::size_t j = coordinate.size(); j; --j) {
        if (++coordinate[j - 1] < description.output.shape[j - 1])
          break;
        coordinate[j - 1] = 0;
      }
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
OperationDefinition reduction_operation(const std::string& key,
                                        ReductionKind kind,
                                        SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {{"axes", OperationParameterType::String}};
  if (kind == ReductionKind::Sum || kind == ReductionKind::Mean ||
      kind == ReductionKind::Variance || kind == ReductionKind::Std)
    traits.parameter_schema.push_back(
        {"dtype", OperationParameterType::String});
  if (kind == ReductionKind::Variance || kind == ReductionKind::Std)
    traits.parameter_schema.insert(traits.parameter_schema.begin() + 1,
                                   {"ddof", OperationParameterType::Int64});
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Whole;
  traits.workspace_bytes =
      kind == ReductionKind::Count ? 0 : sizeof(ReductionState);
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto resolved = metadata(kind, inputs[0], parameters);
    if (!resolved.ok())
      return Answer(resolved.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = resolved.value().output;
    if (kind == ReductionKind::Count) {
      result.maximum_output_payload_bytes = 8;
      result.preserve_output_views = true;
      result.input_indices = std::vector<std::uint32_t>{};
    }
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.callback = [kind, profile](const OperationInvocation& call) {
    return execute_reduction(call, kind, profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_reductions(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (auto kind :
         {ReductionKind::Sum, ReductionKind::Minimum, ReductionKind::Maximum,
          ReductionKind::Mean, ReductionKind::Count, ReductionKind::Variance,
          ReductionKind::Std}) {
      auto status = registry->register_operation(reduction_operation(
          std::string("numeric.") + operation_name(kind) + profile.first, kind,
          profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
