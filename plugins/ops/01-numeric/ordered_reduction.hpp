#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "00-foundation/numeric_common.hpp"
#include "01-numeric/ordered_result.hpp"
#include "plugin/port_validation.hpp"

namespace ps::plugin_internal::numeric_ops {
/** @brief Exact rectangular decomposition of one logical row-major interval.
 * @note At most two boundary paths through rank are needed; there is no
 * bounding-box overread or iteration through individual samples.
 */
inline Result<Footprint> ordered_range(const std::vector<std::uint64_t>& shape,
                                       std::uint64_t begin, std::uint64_t end,
                                       const FootprintLimits& limits) {
  std::vector<std::uint64_t> strides(shape.size(), 1);
  for (std::size_t axis = shape.size(); axis > 1; --axis) {
    if (strides[axis - 1] > UINT64_MAX / shape[axis - 1])
      return Result<Footprint>(Status{ErrorCode::ResourceExhausted, {}});
    strides[axis - 2] = strides[axis - 1] * shape[axis - 1];
  }
  std::vector<Region> regions;
  while (begin < end) {
    std::vector<RegionDimension> dimensions;
    for (std::size_t axis = 0; axis < shape.size(); ++axis)
      dimensions.push_back({begin / strides[axis] % shape[axis], 1});
    std::size_t axis = 0;
    while (axis + 1 < shape.size() &&
           (begin % strides[axis] || strides[axis] > end - begin))
      ++axis;
    dimensions[axis].extent = std::min(shape[axis] - dimensions[axis].offset,
                                       (end - begin) / strides[axis]);
    const auto count = dimensions[axis].extent * strides[axis];
    for (std::size_t tail = axis + 1; tail < shape.size(); ++tail)
      dimensions[tail] = {0, shape[tail]};
    regions.emplace_back(std::move(dimensions));
    begin += count;
  }
  return Footprint::from_regions(shape, std::move(regions), limits);
}
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
  bool variance, ready = false;
  std::uint64_t block, total, channels, cursor = 0, end = 0;
  unsigned pass;
  double sum = 0, mean = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (phase.query.tensor_outputs && phase.query.tensor_outputs->empty())
      return ordered_empty_output(phase);
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Result<ResultProgramPoll>(Status{
          ErrorCode::InvalidArgument, "numeric environment unavailable"});
    const auto& spec = phase.query.inputs[0].result_schema->tensors[0];
    const ValueDescriptor input{spec.descriptor.element_type,
                                spec.sample_shape()};
    if (ready) {
      // Supply has already validated each declared typed fragment. Validation
      // pass needs no extra sample read and retains no payload after this poll.
      if (pass != 0) {
        auto advanced = advance(phase);
        if (!advanced.ok())
          return Result<ResultProgramPoll>(advanced.status());
        auto value = ordered_read_state(advanced.value(), 0);
        if (!value.ok())
          return Result<ResultProgramPoll>(value.status());
        sum = value.value();
      }

      cursor = end;
      if (cursor == total) {
        if (pass == 0) {
          pass = 1;
          cursor = 0;
        } else if (pass == 1 && variance) {
          mean = sum / static_cast<double>(total);
          sum = 0;
          pass = 2;
          cursor = 0;
        } else {
          const double result = sum / static_cast<double>(total);
          auto relation = ResultRelation::cartesian(
              phase.resources, 1,
              {0, 5, 0, total, ResultSupportTarget::Tensor, 0});
          if (!relation.ok())
            return Result<ResultProgramPoll>(relation.status());
          return ordered_output(phase, Region::whole({1}), result,
                                relation.take_value());
        }
      }
    }
    // Round the block's sample count up to full image pixels. Arithmetic still
    // visits every channel in the original logical order.
    const auto count =
        std::min(total - cursor, ((block - 1) / channels + 1) * channels);
    end = cursor + count;
    auto charged = phase.consume_work(1 + 2 * input.shape.size());
    if (!charged.ok())
      return Result<ResultProgramPoll>(charged);
    const auto rank = input.shape.size();
    auto scratch = phase.resources.reserve(ResourceCapacity::host(
        (2 * rank + 1) * (sizeof(Region) + rank * sizeof(RegionDimension)) +
            rank * sizeof(std::uint64_t),
        (2 * rank + 1) * (sizeof(Region) + rank * sizeof(RegionDimension)) +
            rank * sizeof(std::uint64_t)));
    if (!scratch.ok())
      return Result<ResultProgramPoll>(scratch.status());
    auto samples =
        ordered_range(input.shape, cursor, end, ordered_limits(phase));
    if (!samples.ok())
      return Result<ResultProgramPoll>(samples.status());
    ready = true;
    ResultProgramNeed need;
    need.tensors.push_back({0, 0, samples.take_value(), pass == 0 ? 4U : 5U});
    return Result<ResultProgramPoll>(std::move(need));
  }
  Result<ResultRef> advance(const ResultProgramPhase& phase) const {
    const auto& spec = phase.query.inputs[0].result_schema->tensors[0];
    const ValueDescriptor input{spec.descriptor.element_type,
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
  traits.input_schema[0].kind = OperationPortKind::Result;
  traits.input_schema[0].element_type_mask = 12;
  traits.requires_metadata_specialization = true;
  set_whole_tensor_output(traits, ElementType::Float64,
                          sizeof(OrderedReductionState));
  traits.outputs[0].requires_dense_output = true;
  traits.outputs[0].key = "value";
  traits.outputs[0].region_rule = OperationRegionRule::Dependency;
  traits.outputs[0].continuation_bytes = sizeof(OrderedReductionState);
  traits.workspace_bytes = 24;
  traits.outputs[0].maximum_dependency_stages = 1048576;
  traits.parameter_schema = {
      {"block_size", OperationParameterType::Int64, false, true, 1, 65536}};
  operation.specialize_metadata =
      [](const auto& inputs,
         const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    if (!inputs[0].result_schema->fields.empty())
      return Result<std::vector<OperationOutputSpecialization>>(Status{
          ErrorCode::TypeMismatch, "ordered reduction requires one tensor"});
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
    const auto channels =
        input_internal::tuple_channel_axis(input.descriptor, input.facets);
    const bool typed = std::any_of(
        input.facets.begin(), input.facets.end(), [](const auto& facet) {
          return input_internal::typed_facet(facet.key);
        });
    return ResultContinuation::make<OrderedReductionState>(
        allocator, variance, block, input.sample_count().value(),
        channels ? input.descriptor.shape[*channels] : 1, typed);
  };
  return operation;
}
}  // namespace ps::plugin_internal::numeric_ops
