#pragma once

#include <cstring>
#include <utility>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal::multi_output {
inline std::vector<std::uint64_t> coordinate(const DependencyPhase& phase) {
  std::vector<std::uint64_t> result;
  for (const auto& dim : phase.query.observations.boxes()[0].dimensions())
    result.push_back(dim.offset);
  return result;
}
inline Result<DependencyPoll> need(const DependencyPhase& phase,
                                   std::vector<DependencyNeed> inputs) {
  return Result<DependencyPoll>(
      DependencyNeedBatch{{{coordinate(phase), std::move(inputs)}}, {}});
}
inline Result<DependencyPoll> finish(const DependencyPhase& phase,
                                     Value value) {
  auto result = ValueFragments::create(
      phase.query.output.descriptor, phase.query.output.facets,
      phase.query.outputs, {std::move(value)}, phase.sets);
  if (!result.ok())
    return Result<DependencyPoll>(result.status());
  return Result<DependencyPoll>(result.take_value());
}
inline Result<DependencyPoll> finish(const DependencyPhase& phase,
                                     const void* data, std::size_t size) {
  auto allocated =
      MutableValue::allocate(phase.query.output.descriptor,
                             phase.query.outputs.boxes()[0], phase.allocator);
  if (!allocated.ok())
    return Result<DependencyPoll>(allocated.status());
  auto output = allocated.take_value();
  if (output.size() != size)
    return Result<DependencyPoll>(
        Status{ErrorCode::InvalidArgument, "output byte count mismatch"});
  std::memcpy(output.data(), data, size);
  auto value = std::move(output).publish(phase.query.output.facets);
  if (!value.ok())
    return Result<DependencyPoll>(value.status());
  return finish(phase, value.take_value());
}
inline OperationExtent input_axis(std::uint32_t input, std::uint32_t axis,
                                  std::uint64_t divisor = 1) {
  OperationExtent result;
  result.source = OperationExtentSource::InputAxis;
  result.input = input;
  result.axis = axis;
  result.divisor = divisor;
  return result;
}
}  // namespace ps::plugin_internal::multi_output
