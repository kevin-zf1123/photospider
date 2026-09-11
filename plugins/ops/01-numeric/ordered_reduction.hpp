#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "00-foundation/numeric_common.hpp"
#include "data/input_validation.hpp"

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
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Result<DependencyPoll>(Status{ErrorCode::InvalidArgument,
                                           "numeric environment unavailable"});
    const auto& input = phase.query.inputs[0].descriptor;
    if (ready) {
      // Supply has already validated each declared typed fragment. Validation
      // pass needs no extra sample read and retains no payload after this poll.
      if (pass != 0) {
        std::vector<std::uint64_t> at(input.shape.size());
        for (auto index = cursor; index < end; ++index) {
          auto remainder = index;
          for (std::size_t axis = at.size(); axis; --axis) {
            at[axis - 1] = remainder % input.shape[axis - 1];
            remainder /= input.shape[axis - 1];
          }
          double value = 0;
          Status status;
          if (input.element_type == ElementType::Float32) {
            float sample = 0;
            status = phase.read(0, at, &sample, 4);
            value = sample;
          } else {
            status = phase.read(0, at, &value, 8);
          }
          if (!status.ok())
            return Result<DependencyPoll>(status);
          if (pass == 1) {
            if (!std::isfinite(value))
              return Result<DependencyPoll>(numeric_internal::numeric_failure(
                  index, "reduction input is nonfinite"));
            sum += value;
            if (!std::isfinite(sum))
              return Result<DependencyPoll>(numeric_internal::numeric_failure(
                  index, "reduction sum overflow"));
          } else {
            const double difference = value - mean;
            const double square = difference * difference;
            sum += square;
            if (!std::isfinite(sum))
              return Result<DependencyPoll>(numeric_internal::numeric_failure(
                  index, "variance overflow"));
          }
        }
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
          auto made =
              MutableValue::allocate(phase.query.output.descriptor,
                                     Region::whole({1}), phase.allocator);
          if (!made.ok())
            return Result<DependencyPoll>(made.status());
          auto writer = made.take_value();
          std::memcpy(writer.data(), &result, 8);
          auto value = std::move(writer).publish();
          if (!value.ok())
            return Result<DependencyPoll>(value.status());
          auto fragments = ValueFragments::create(
              phase.query.output.descriptor, phase.query.output.facets,
              phase.query.outputs, {value.take_value()}, phase.sets);
          if (!fragments.ok())
            return Result<DependencyPoll>(fragments.status());
          return Result<DependencyPoll>(fragments.take_value());
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
      return Result<DependencyPoll>(charged);
    auto samples = ordered_range(input.shape, cursor, end, phase.sets);
    if (!samples.ok())
      return Result<DependencyPoll>(samples.status());
    ready = true;
    return Result<DependencyPoll>(DependencyNeedBatch{
        {{{0}, {{0, pass == 0 ? 4U : 5U, samples.take_value(), {}}}}},
        {}});
  }
};
inline OperationDefinition ordered_reduction(const char* key, bool variance) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].element_type_mask = 12;
  traits.requires_dense_output = true;
  traits.region_rule = OperationRegionRule::Dependency;
  traits.dependency_version = 1;
  traits.continuation_bytes = sizeof(OrderedReductionState);
  traits.maximum_dependency_stages = 1048576;
  traits.parameter_schema = {
      {"block_size", OperationParameterType::Int64, false, true, 1, 65536}};
  operation.validate_dependency = [](const auto& inputs, const auto&) {
    const auto count =
        Region::whole(inputs[0].descriptor.shape).element_count();
    return count.ok() ? Status::success() : count.status();
  };
  operation.start_dependency = [variance](const DependencyQuery& query,
                                          const BufferAllocator& allocator) {
    const auto found = query.parameters.find("block_size");
    const auto block =
        found == query.parameters.end()
            ? 64U
            : static_cast<std::uint64_t>(std::get<std::int64_t>(found->second));
    const auto& input = query.inputs[0];
    const bool image = std::any_of(
        input.facets.begin(), input.facets.end(),
        [](const auto& facet) { return facet.key == "photospider.image"; });
    const bool typed = std::any_of(input.facets.begin(), input.facets.end(),
                                   [](const auto& facet) {
                                     return facet.key == "photospider.image" ||
                                            facet.key == "photospider.semantic";
                                   });
    return DependencyContinuation::make<OrderedReductionState>(
        allocator, variance, block,
        Region::whole(input.descriptor.shape).element_count().value(),
        image ? input.descriptor.shape.back() : 1, typed);
  };
  return operation;
}
}  // namespace ps::plugin_internal::numeric_ops
