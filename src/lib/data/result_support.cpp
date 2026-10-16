#include "data/result_support.hpp"

#include <algorithm>
#include <utility>

#include "photospider/core/checked_math.hpp"

namespace ps::data_internal {
Status invalid_schema() {
  return Status{ErrorCode::TypeMismatch,
                "invalid structured result schema or association"};
}
Status unavailable() {
  return Status{ErrorCode::NotFound, "structured result range is incomplete"};
}
Result<std::uint64_t> scaled(const ResultExtent& extent, std::uint64_t count) {
  count = count / extent.divisor + (count % extent.divisor != 0);
  if (!core_internal::can_add(count, extent.offset))
    return Result<std::uint64_t>(
        Status{ErrorCode::ResourceExhausted, "result extent overflow"});
  return Result<std::uint64_t>(count + extent.offset);
}
Status tensor_topology(const ResultTensorSpec& spec) {
  const auto rank = spec.descriptor.shape.size();
  if (!rank || !core_internal::can_add(rank, spec.batch_axes.size(), 8) ||
      spec.atomic_trailing_axes > rank ||
      std::any_of(spec.descriptor.shape.begin(), spec.descriptor.shape.end(),
                  [](auto n) { return !n; }) ||
      std::any_of(spec.batch_axes.begin(), spec.batch_axes.end(),
                  [](auto n) { return !n; }))
    return invalid_schema();
  const auto dtype = static_cast<std::uint32_t>(spec.descriptor.element_type);
  if (dtype < 1 || dtype > 7)
    return invalid_schema();
  if (spec.layout.spatial) {
    const auto& layout = spec.layout;
    const auto free_axes = rank - spec.atomic_trailing_axes;
    if (rank < 2 || layout.height_axis >= free_axes ||
        layout.width_axis >= free_axes ||
        layout.height_axis == layout.width_axis ||
        (layout.channel_axis && (*layout.channel_axis >= rank ||
                                 *layout.channel_axis == layout.height_axis ||
                                 *layout.channel_axis == layout.width_axis)))
      return invalid_schema();
  }
  return Status::success();
}
}  // namespace ps::data_internal
