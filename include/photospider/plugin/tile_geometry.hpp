#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include "photospider/data/footprint.hpp"
#include "photospider/data/tiles.hpp"

namespace ps::sdk {
namespace tile_helpers_detail {
// Conversion lives at the caller/operation boundary. The coordinator only
// consumes explicit indices; it never enlarges arbitrary sample rectangles.
template <class Iterator>
Result<TileSet> regions(const ResourceBudget& root, const TileGrid& grid,
                        Iterator first, Iterator last) try {
  using Answer = Result<TileSet>;
  if (!grid.tile_count() || !grid.rank() || grid.rank() > 8)
    return Answer(Status{ErrorCode::InvalidArgument, "invalid tile grid"});
  std::array<std::uint32_t, 8> axes{};
  std::array<std::uint64_t, 8> sizes{}, strides{};
  std::size_t n = 0;
  for (std::uint32_t axis = 0; axis < grid.rank(); ++axis)
    if (grid.batch_axis_mask() & (1U << axis)) {
      axes[n] = axis;
      sizes[n++] = grid.shape()[axis];
    }
  if (grid.height_axis() < grid.rank()) {
    axes[n] = grid.height_axis();
    sizes[n++] = grid.tile_rows();
  }
  if (grid.width_axis() < grid.rank()) {
    axes[n] = grid.width_axis();
    sizes[n++] = grid.tile_columns();
  }
  std::uint64_t stride = 1;
  for (auto j = n; j-- > 0;) {
    strides[j] = stride;
    stride *= sizes[j];
  }
  ResourceVector<TileInterval> intervals{ResourceAllocator<TileInterval>(root)};
  for (auto it = first; it != last; ++it) {
    auto work = root.consume({grid.rank() + n + 1});
    if (!work.ok())
      return Answer(work);
    if (it->rank() != grid.rank())
      return Answer(Status{ErrorCode::InvalidArgument, "tile region rank"});
    const auto& dims = it->dimensions();
    for (std::size_t axis = 0; axis < dims.size(); ++axis)
      if (dims[axis].offset > grid.shape()[axis] ||
          dims[axis].extent > grid.shape()[axis] - dims[axis].offset)
        return Answer(Status{ErrorCode::InvalidArgument, "tile region bounds"});
    if (it->empty())
      continue;
    std::array<std::uint64_t, 8> begin{}, end{}, at{};
    for (std::size_t j = 0; j < n; ++j) {
      const auto d = dims[axes[j]];
      const auto scale =
          grid.batch_axis_mask() & (1U << axes[j]) ? 1 : grid.tile_extent();
      begin[j] = d.offset / scale;
      end[j] = (d.offset + d.extent - 1) / scale + 1;
      at[j] = begin[j];
    }
    auto inner = n ? n - 1 : 0;
    while (inner && !begin[inner] && end[inner] == sizes[inner])
      --inner;
    std::uint64_t count = 1;
    for (std::size_t j = 0; j < inner; ++j) {
      if (end[j] - begin[j] > 65536 / count)
        return Answer(
            Status{ErrorCode::ResourceExhausted, "tile interval count"});
      count *= end[j] - begin[j];
    }
    if (count > 65536 - intervals.size())
      return Answer(
          Status{ErrorCode::ResourceExhausted, "tile interval count"});
    work = root.consume({count * (n + 1)});
    if (!work.ok())
      return Answer(work);
    intervals.reserve(intervals.size() + count);
    for (std::uint64_t i = 0; i < count; ++i) {
      std::uint64_t first_tile = 0;
      for (std::size_t j = 0; j < n; ++j)
        first_tile += at[j] * strides[j];
      intervals.push_back(
          {first_tile,
           first_tile +
               (n ? (end[inner] - begin[inner]) * strides[inner] : 1)});
      for (auto j = inner; j-- > 0;) {
        if (++at[j] < end[j])
          break;
        at[j] = begin[j];
      }
    }
  }
  return TileSet::from_intervals(root, intervals.data(), intervals.size());
} catch (const std::bad_alloc&) {
  return Result<TileSet>(Status{ErrorCode::ResourceExhausted, {}});
}
}  // namespace tile_helpers_detail

/** @brief Converts one logical sample coordinate into its compiled tile.
 * Includes batch coordinates and ignores no bounds: every non-spatial axis is
 * checked even though its complete extent belongs to the tile. No allocation
 * or pixel access occurs. Callers account work when converting collections.
 */
inline Result<TileIndex> tile_for_coordinate(const TileGrid& grid,
                                             const std::uint64_t* at,
                                             std::size_t rank) {
  if (!grid.tile_count() || !at || rank != grid.rank())
    return Result<TileIndex>(
        Status{ErrorCode::InvalidArgument, "invalid tile coordinate rank"});
  for (std::size_t axis = 0; axis < rank; ++axis)
    if (at[axis] >= grid.shape()[axis])
      return Result<TileIndex>(
          Status{ErrorCode::InvalidArgument, "tile coordinate outside grid"});
  TileIndex batch = 0;
  for (std::size_t axis = 0; axis < rank; ++axis)
    if (grid.batch_axis_mask() & (1U << axis))
      batch = batch * grid.shape()[axis] + at[axis];
  const auto row = grid.height_axis() < rank
                       ? at[grid.height_axis()] / grid.tile_extent()
                       : 0;
  const auto column =
      grid.width_axis() < rank ? at[grid.width_axis()] / grid.tile_extent() : 0;
  return Result<TileIndex>(
      (batch * grid.tile_rows() + row) * grid.tile_columns() + column);
}

/** @brief Converts a logical sample rectangle to all intersecting tiles.
 * @note Partial non-spatial axes still select complete tiles. Batch intervals
 * select only their own coordinates. Empty rectangles return an empty set.
 * Work and temporary/final index storage are charged to root. No pixel reads.
 * @return InvalidArgument for a rank/domain mismatch, ResourceExhausted for
 * Root work/capacity exhaustion. Owners remain valid after root destruction.
 */
inline Result<TileSet> tiles_for_region(const ResourceBudget& root,
                                        const TileGrid& grid,
                                        const Region& region) {
  return tile_helpers_detail::regions(root, grid, &region, &region + 1);
}
/** @brief Converts an exact sample set at the operation/SDK boundary.
 * @note Overlapping tiles are deduplicated. Work scales with supplied boxes
 * and their intersecting tiles, not the size of the producer's entire grid.
 */
inline Result<TileSet> tiles_for_samples(const ResourceBudget& root,
                                         const TileGrid& grid,
                                         const Footprint& samples) {
  if (!samples.valid() || samples.shape().size() != grid.rank())
    return Result<TileSet>(
        Status{ErrorCode::InvalidArgument, "tile sample domain mismatch"});
  for (std::size_t axis = 0; axis < grid.rank(); ++axis)
    if (samples.shape()[axis] != grid.shape()[axis])
      return Result<TileSet>(
          Status{ErrorCode::InvalidArgument, "tile sample domain mismatch"});
  return tile_helpers_detail::regions(root, grid, samples.boxes().begin(),
                                      samples.boxes().end());
}
}  // namespace ps::sdk
