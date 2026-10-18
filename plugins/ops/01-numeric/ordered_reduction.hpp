#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "01-numeric/ordered_result.hpp"
#include "common/floating_environment.hpp"
#include "common/numeric_common.hpp"
#include "photospider/plugin/tile_helpers.hpp"

namespace ps::plugin_internal::numeric_ops {
/** @brief Strict binary64 carry; stage boundaries never reassociate sums.
 * @note Typed source validation completes before arithmetic, matching the
 * previous Whole input validation. Pass two retains the exact pass-one mean.
 * State and live input/output storage use the existing host allocator.
 */
struct OrderedReductionState final {
  explicit OrderedReductionState(bool variance, std::uint64_t block,
                                 std::uint64_t total, std::uint64_t channels,
                                 bool typed)
      : variance(variance),
        block(block),
        total(total),
        channels(channels),
        pass(typed ? 0 : 1) {}
  bool variance;
  std::uint64_t block, total, channels, cursor = 0, end = 0;
  unsigned pass;
  double sum = 0, mean = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (phase.query.tensor_outputs && phase.query.tensor_outputs->empty())
      return ordered_empty_output(phase);
    plugin_internal::FloatingEnvironment environment;
    if (!environment.active())
      return Result<ResultProgramPoll>(Status{
          ErrorCode::InvalidArgument, "numeric environment unavailable"});
    // The fixed grant is validated before entering the operation.
    if (pass == 0)
      pass = 1;
    for (;;) {
      // Preserve the original sample order and channel-aligned block sizes.
      const auto count =
          std::min(total - cursor, ((block - 1) / channels + 1) * channels);
      end = cursor + count;
      auto advanced = advance(phase);
      if (!advanced.ok())
        return Result<ResultProgramPoll>(advanced.status());
      auto value = ordered_read_state(advanced.value(), 0);
      if (!value.ok())
        return Result<ResultProgramPoll>(value.status());
      sum = value.value();
      cursor = end;
      if (cursor < total)
        continue;
      if (pass == 1 && variance) {
        mean = sum / static_cast<double>(total);
        sum = 0;
        pass = 2;
        cursor = 0;
      } else {
        return ordered_output(phase, Region::whole({1}),
                              sum / static_cast<double>(total));
      }
    }
  }
  Result<ResultRef> advance(const ResultProgramPhase& phase) const {
    const auto& spec = phase.query.inputs[0].result_schema->tensors[0];
    const TensorDescriptor input{spec.descriptor.element_type,
                                 spec.sample_shape()};
    const auto state = [&](double value) {
      return ordered_state(phase, {value, mean});
    };
    auto incoming = state(sum);
    if (!incoming.ok())
      return incoming;
    return phase.block(
        pass, cursor, end, 1, incoming.value(), [&]() -> Result<ResultRef> {
          double outgoing = sum;
          std::vector<std::uint64_t> at(input.shape.size());
          for (auto index = cursor; index < end; ++index) {
            auto remainder = index;
            for (std::size_t axis = at.size(); axis; --axis) {
              at[axis - 1] = remainder % input.shape[axis - 1];
              remainder /= input.shape[axis - 1];
            }
            double value = 0;
            auto status = phase.consume_work(1);
            if (!status.ok())
              return Result<ResultRef>(status);
            if (input.element_type == ElementType::Float32) {
              float sample = 0;
              status = phase.read_tensor(0, 0, at, &sample, 4);
              value = sample;
            } else {
              status = phase.read_tensor(0, 0, at, &value, 8);
            }
            if (!status.ok())
              return Result<ResultRef>(status);
            if (pass == 1) {
              if (!std::isfinite(value))
                return Result<ResultRef>(numeric_internal::numeric_failure(
                    index, "reduction input is nonfinite"));
              outgoing += value;
              if (!std::isfinite(outgoing))
                return Result<ResultRef>(numeric_internal::numeric_failure(
                    index, "reduction sum overflow"));
            } else {
              const double difference = value - mean;
              const double square = difference * difference;
              outgoing += square;
              if (!std::isfinite(outgoing))
                return Result<ResultRef>(numeric_internal::numeric_failure(
                    index, "variance overflow"));
            }
          }
          return state(outgoing);
        });
  }
};
inline OperationDefinition ordered_reduction(const char* key, bool variance) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);

  traits.input_schema[0].element_type_mask = 12;
  traits.requires_metadata_specialization = true;
  set_whole_tensor_output(traits, ElementType::Float64,
                          sizeof(OrderedReductionState));
  traits.outputs[0].key = "value";
  traits.outputs[0].continuation_bytes = sizeof(OrderedReductionState);
  traits.workspace_bytes = 24;
  traits.outputs[0].maximum_dependency_stages = 1048576;
  traits.parameter_schema = {
      {"block_size", OperationParameterType::Int64, false, true, 1, 65536}};
  operation.specialize_metadata =
      [](const auto& inputs,
         const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    auto count = inputs[0].result_schema->tensors[0].sample_count();
    if (!count.ok())
      return Result<std::vector<OperationOutputSpecialization>>(count.status());
    OperationOutputSpecialization output;
    output.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_tensor_schema(ElementType::Float64, {1}));
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  operation.start_result = [variance](const ResultProgramQuery& query,
                                      const BufferAllocator& allocator) {
    const auto found = query.parameters.find("block_size");
    const auto block =
        found == query.parameters.end()
            ? 64U
            : static_cast<std::uint64_t>(std::get<std::int64_t>(found->second));
    const auto& input = query.inputs[0].result_schema->tensors[0];
    std::uint64_t channels = 1;
    for (std::size_t axis =
             input.descriptor.shape.size() - input.atomic_trailing_axes;
         axis < input.descriptor.shape.size(); ++axis)
      channels *= input.descriptor.shape[axis];
    return ResultContinuation::make<OrderedReductionState>(
        allocator, variance, block, input.sample_count().value(), channels,
        false);
  };
  return operation;
}
}  // namespace ps::plugin_internal::numeric_ops
