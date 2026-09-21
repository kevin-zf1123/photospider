#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_quantile.hpp"
#include "01-numeric/stable_order.hpp"
#include "data/input_validation.hpp"
#include "photospider/data/semantic.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
Status shape_error(const char* message) {
  return Status{ErrorCode::TypeMismatch,
                message,
                FailureReason::None,
                {FailureOrigin::Schema, FailureScope::Unspecified}};
}

Result<std::uint32_t> ordering_axis(
    const ValueDescriptor& descriptor,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<std::uint32_t>;
  if (descriptor.shape.empty() || descriptor.shape.size() > 8)
    return Answer(shape_error("ordering requires rank 1..8"));
  std::uint64_t count = 1;
  for (auto extent : descriptor.shape) {
    if (!extent || extent > (UINT64_C(1) << 40) / count)
      return Answer(shape_error("ordering exceeds 2^40 elements"));
    count *= extent;
  }
  const auto axis = std::get<std::int64_t>(parameters.at("axis"));
  if (axis < 0 || static_cast<std::uint64_t>(axis) >= descriptor.shape.size())
    return Answer(
        numeric_ops::array_parameter_error("ordering axis outside rank"));
  return Answer(static_cast<std::uint32_t>(axis));
}
struct OrderingState final {
  numeric_ops::StableOrderWorkspace ordering;
  numeric_ops::ExactQuantile arithmetic;
  explicit OrderingState(SequenceProfile profile) : arithmetic(profile) {}
};
Result<Value> execute_ordering(const OperationInvocation& call, bool quantile,
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
    const auto& input = call.inputs[0];
    const auto& shape = input.descriptor().shape;
    auto selected = ordering_axis(input.descriptor(), call.parameters);
    if (!selected.ok())
      return Answer(selected.status());
    const auto axis = selected.value();
    const auto count = shape[axis];
    const auto type = input.descriptor().element_type;
    auto output_shape = shape;
    auto target = call.output_index == 1 ? ElementType::Int64 : type;
    numeric_ops::QuantilePosition position;
    if (quantile) {
      output_shape[axis] = 1;
      target = std::get<std::string>(call.parameters.at("dtype")) == "float32"
                   ? ElementType::Float32
                   : ElementType::Float64;
      if (count > 1) {
        const auto& probability = call.inputs[1];
        auto at = probability.byte_address({0});
        if (!at.ok())
          return Answer(at.status());
        std::uint64_t bits = 0;
        std::memcpy(&bits, probability.bytes().data() + at.value(),
                    Value::element_size(probability.descriptor().element_type));
        auto found = numeric_ops::quantile_position(
            bits, probability.descriptor().element_type, count);
        if (!found.ok()) {
          auto failure = found.status();
          std::array<char, 128> message{};
          std::snprintf(message.data(), message.size(),
                        "InvalidQuantileProbability: port=1 bits=0x%016" PRIx64
                        "; require finite q in [0,1]",
                        bits);
          failure.message = message.data();
          failure.detail.scope = FailureScope::Run;
          return Answer(failure);
        }
        position = found.value();
      }
    }
    auto scratch = call.allocator.allocate(sizeof(OrderingState));
    if (!scratch.ok())
      return Answer(scratch.status());
    auto buffer = scratch.take_value();
    std::unique_ptr<OrderingState, void (*)(OrderingState*)> state(
        new (buffer.data()) OrderingState(profile),
        [](OrderingState* item) { item->~OrderingState(); });
    auto allocated = MutableValue::allocate({target, output_shape},
                                            call.output_region, call.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto output = allocated.take_value();
    std::vector<std::uint64_t> coordinate(shape.size(), 0),
        source(shape.size(), 0);
    std::uint64_t lines = 1;
    for (std::size_t j = 0; j < shape.size(); ++j)
      if (j != axis)
        lines *= shape[j];
    const auto read = [&](std::uint64_t index, std::uint64_t* bits) {
      auto charged = work(shape.size() + 1);
      if (!charged.ok())
        return charged;
      source = coordinate;
      source[axis] = index;
      auto at = input.byte_address(source);
      if (!at.ok())
        return at.status();
      *bits = 0;
      std::memcpy(bits, input.bytes().data() + at.value(),
                  Value::element_size(type));
      return Status::success();
    };
    for (std::uint64_t line = 0; line < lines; ++line) {
      auto permutation =
          state->ordering.build(count, type, profile, work, read);
      if (!permutation.ok())
        return Answer(permutation.status());
      const auto& indices = permutation.value();
      const auto store = [&](std::uint64_t bits) {
        std::uint64_t offset = 0;
        for (std::size_t j = 0; j < output_shape.size(); ++j)
          offset = offset * output_shape[j] + coordinate[j];
        std::array<std::uint64_t, 4> replicas{};
        numeric_ops::select_words(replicas.data(), bits, bits, 1, profile);
        std::memcpy(output.data() + offset * Value::element_size(target),
                    replicas.data(), Value::element_size(target));
        return work(1);
      };
      if (!quantile) {
        for (std::uint64_t j = 0; j < count; ++j) {
          coordinate[axis] = j;
          std::uint64_t bits = indices[j];
          if (call.output_index == 0) {
            status = read(indices[j], &bits);
            if (!status.ok())
              return Answer(status);
          }
          status = store(bits);
          if (!status.ok())
            return Answer(status);
        }
      } else {
        std::uint64_t nan_begin = count;
        if (type == ElementType::Float32 || type == ElementType::Float64) {
          std::uint64_t low = 0, high = count;
          while (low < high) {
            const auto middle = low + (high - low) / 2;
            std::uint64_t bits = 0;
            status = read(indices[middle], &bits);
            if (!status.ok())
              return Answer(status);
            if (numeric_ops::BinaryParts::decode(bits,
                                                 type == ElementType::Float32)
                    .nan)
              high = middle;
            else
              low = middle + 1;
          }
          nan_begin = low;
        }
        std::uint64_t bits = 0;
        if (nan_begin < count) {
          status = read(indices[nan_begin], &bits);
          if (!status.ok())
            return Answer(status);
          bits = numeric_ops::converted_nan(bits, type, target);
        } else {
          std::uint64_t a = 0, b = 0;
          status = read(indices[position.index], &a);
          if (status.ok() && position.fractional())
            status = read(indices[position.index + 1], &b);
          if (!status.ok())
            return Answer(status);
          auto calculated =
              state->arithmetic.finish(a, b, type, target, position, work);
          if (!calculated.ok())
            return Answer(calculated.status());
          bits = calculated.value();
        }
        status = store(bits);
        if (!status.ok())
          return Answer(status);
      }
      coordinate[axis] = 0;
      for (std::size_t j = shape.size(); j; --j)
        if (j - 1 != axis) {
          if (++coordinate[j - 1] < shape[j - 1])
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
OperationDefinition ordering_operation(const std::string& key, bool quantile,
                                       SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = quantile ? 2 : 1;
  traits.input_schema.resize(traits.input_count);
  if (quantile) {
    traits.input_schema[1].rank = 1;
    traits.input_schema[1].element_type_mask = 12;
  }
  traits.requires_metadata_specialization = true;
  traits.workspace_bytes = sizeof(OrderingState);
  traits.parameter_schema = {{"axis", OperationParameterType::Int64}};
  if (quantile)
    traits.parameter_schema.push_back(
        {"dtype", OperationParameterType::String});
  traits.outputs.resize(quantile ? 1 : 2);
  for (std::size_t j = 0; j < traits.outputs.size(); ++j) {
    auto& output = traits.outputs[j];
    output.key = j ? "indices" : "values";
    output.shape_rule = OperationShapeRule::Fixed;
    output.fixed_output_shape = {1};
    output.region_rule = OperationRegionRule::Whole;
    output.requires_dense_output = true;
  }
  operation.specialize_metadata = [quantile, profile](const auto& inputs,
                                                      const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto axis = ordering_axis(inputs[0].descriptor, parameters);
    if (!axis.ok())
      return Answer(axis.status());
    auto shape = inputs[0].descriptor.shape;
    auto type = inputs[0].descriptor.element_type;
    if (quantile) {
      if (inputs[1].descriptor.shape != std::vector<std::uint64_t>{1})
        return Answer(shape_error("quantile q requires scalar [1]"));
      const auto& dtype = std::get<std::string>(parameters.at("dtype"));
      if (dtype != "float32" && dtype != "float64")
        return Answer(numeric_ops::array_parameter_error(
            "quantile requires floating dtype"));
      type = dtype == "float32" ? ElementType::Float32 : ElementType::Float64;
      shape[axis.value()] = 1;
    }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    std::vector<OperationOutputSpecialization> results(quantile ? 1 : 2);
    results[0].metadata.descriptor = {type, shape};
    if (!quantile)
      results[1].metadata.descriptor = {ElementType::Int64, shape};
    if (quantile && inputs[0].descriptor.shape[axis.value()] == 1)
      results[0].input_indices = std::vector<std::uint32_t>{0};
    return Answer(std::move(results));
  };
  operation.callback = [quantile, profile](const OperationInvocation& call) {
    return execute_ordering(call, quantile, profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_ordering(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool quantile : {false, true}) {
      auto status = registry->register_operation(ordering_operation(
          std::string(quantile ? "numeric.quantile" : "array.sort") +
              profile.first,
          quantile, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
