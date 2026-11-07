#pragma once

#include <cstdint>

#include "photospider/core/checked_math.hpp"
#include "photospider/data/result.hpp"

namespace ps::data_internal {
// Permit cropped / reversed planar views; adjacent planes cannot interleave.
// Singleton axes contribute no physical step.
inline bool planar_view(const ResultTensorSpec& spec, const Region& region,
                        const std::int64_t* strides) {
  if (!spec.layout.spatial)
    return true;
  const auto x = spec.batch_axes.size() + spec.layout.width_axis;
  const auto y = spec.batch_axes.size() + spec.layout.height_axis;
  if (x >= region.rank() || y >= region.rank() || x == y)
    return false;
  std::uint64_t end = TensorSpan::element_size(spec.descriptor.element_type);
  auto axis_ok = [&](std::size_t axis) {
    const auto n = region.dimensions()[axis].extent;
    if (n <= 1)
      return true;
    const auto step = strides[axis];
    const std::uint64_t stride = step < 0
                                     ? 0U - static_cast<std::uint64_t>(step)
                                     : static_cast<std::uint64_t>(step);
    return stride >= end && (axis != x || stride == end) &&
           core_internal::checked_multiply_add(n - 1, stride, end, &end);
  };
  if (!axis_ok(x) || !axis_ok(y))
    return false;
  for (auto axis = region.rank(); axis-- > 0;)
    if (axis != x && axis != y && !axis_ok(axis))
      return false;
  return true;
}
}  // namespace ps::data_internal
