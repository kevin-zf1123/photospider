#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "photospider/plugin/operation_program.hpp"

namespace ps::plugin_internal::fixed_tensor {
inline Region view_region(const physical::Layout& layout) {
  std::vector<RegionDimension> dimensions;
  for (unsigned axis = 0; axis < layout.rank; ++axis)
    dimensions.push_back({layout.origin[axis], layout.shape[axis]});
  return Region(std::move(dimensions));
}
inline TileIndex input_tile(const TileGrid& grid,
                            const std::array<std::uint64_t, 8>& at) {
  TileIndex batch = 0;
  for (unsigned axis = 0; axis < grid.rank(); ++axis)
    if (grid.batch_axis_mask() & (1U << axis))
      batch = batch * grid.shape()[axis] + at[axis];
  const auto row = grid.height_axis() < grid.rank()
                       ? at[grid.height_axis()] / grid.tile_extent()
                       : 0;
  const auto column = grid.width_axis() < grid.rank()
                          ? at[grid.width_axis()] / grid.tile_extent()
                          : 0;
  return (batch * grid.tile_rows() + row) * grid.tile_columns() + column;
}
}  // namespace ps::plugin_internal::fixed_tensor
