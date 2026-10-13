#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

#include "photospider/data/footprint.hpp"
#include "photospider/data/tiles.hpp"
#include "photospider/plugin/result_program.hpp"

namespace ps::sdk {
namespace tile_helpers_detail {
// Conversion lives at the caller/operation boundary. The coordinator only
// consumes explicit indices; it never enlarges arbitrary sample rectangles.
template <class Iterator>
Result<TileSet> regions(const ResourceBudget& root, const TileGrid& grid,
                        Iterator first, Iterator last) try {
  using Answer = Result<TileSet>;
  if (!grid.tile_count() || grid.rank() < 2 || grid.rank() > 8)
    return Answer(Status{ErrorCode::InvalidArgument, "invalid tile grid"});
  std::uint64_t count = 0;
  for (auto it = first; it != last; ++it) {
    auto charged = root.consume({grid.rank() + 1});
    if (!charged.ok())
      return Answer(charged);
    if (it->rank() != grid.rank())
      return Answer(
          Status{ErrorCode::InvalidArgument, "tile Region rank mismatch"});
    const auto& dimensions = it->dimensions();
    for (std::size_t axis = 0; axis < dimensions.size(); ++axis)
      if (dimensions[axis].offset > grid.shape()[axis] ||
          dimensions[axis].extent >
              grid.shape()[axis] - dimensions[axis].offset)
        return Answer(
            Status{ErrorCode::InvalidArgument, "tile Region outside grid"});
    if (it->empty())
      continue;
    const auto& height = dimensions[grid.height_axis()];
    const auto& width = dimensions[grid.width_axis()];
    const auto rows = (height.offset + height.extent - 1) / grid.tile_extent() -
                      height.offset / grid.tile_extent() + 1;
    const auto columns =
        (width.offset + width.extent - 1) / grid.tile_extent() -
        width.offset / grid.tile_extent() + 1;
    // Every factor is a subset of a validated grid whose count fits uint64.
    std::uint64_t units = rows * columns;
    for (std::size_t axis = 0; axis < grid.batch_rank(); ++axis)
      units *= dimensions[axis].extent;
    if (units > std::numeric_limits<std::size_t>::max() - count)
      return Answer(Status{ErrorCode::ResourceExhausted, "tile list overflow"});
    count += units;
  }
  auto charged = root.consume({count});
  if (!charged.ok())
    return Answer(charged);
  ResourceVector<TileIndex> indices{ResourceAllocator<TileIndex>(root)};
  if (count > indices.max_size())
    return Answer(Status{ErrorCode::ResourceExhausted,
                         "tile list exceeds addressable storage"});
  indices.reserve(static_cast<std::size_t>(count));
  for (auto it = first; it != last; ++it) {
    if (it->empty())
      continue;
    const auto& dimensions = it->dimensions();
    const auto& height = dimensions[grid.height_axis()];
    const auto& width = dimensions[grid.width_axis()];
    const auto first_row = height.offset / grid.tile_extent();
    const auto last_row =
        (height.offset + height.extent - 1) / grid.tile_extent();
    const auto first_column = width.offset / grid.tile_extent();
    const auto last_column =
        (width.offset + width.extent - 1) / grid.tile_extent();
    std::uint64_t batches = 1;
    for (std::size_t axis = 0; axis < grid.batch_rank(); ++axis)
      batches *= dimensions[axis].extent;
    for (std::uint64_t batch = 0; batch < batches; ++batch) {
      auto remaining = batch;
      std::array<std::uint64_t, 8> coordinates{};
      for (std::size_t axis = grid.batch_rank(); axis-- > 0;) {
        coordinates[axis] =
            dimensions[axis].offset + remaining % dimensions[axis].extent;
        remaining /= dimensions[axis].extent;
      }
      std::uint64_t index = 0;
      for (std::size_t axis = 0; axis < grid.batch_rank(); ++axis)
        index = index * grid.shape()[axis] + coordinates[axis];
      for (auto row = first_row; row <= last_row; ++row)
        for (auto column = first_column; column <= last_column; ++column)
          indices.push_back(
              (index * grid.tile_rows() + row) * grid.tile_columns() + column);
    }
  }
  return TileSet::from_indices(root, indices.data(), indices.size());
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
  for (std::size_t axis = 0; axis < grid.batch_rank(); ++axis)
    batch = batch * grid.shape()[axis] + at[axis];
  return Result<TileIndex>(
      (batch * grid.tile_rows() + at[grid.height_axis()] / grid.tile_extent()) *
          grid.tile_columns() +
      at[grid.width_axis()] / grid.tile_extent());
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
/** @brief Creates an input Need at the operation boundary.
 * A nonempty sample selection requests complete intersecting spatial tiles.
 * Inputs without a grid request Whole, including all Result members. Empty
 * payload selections are invalid; descriptor_only must be explicit. Compiled
 * grids take precedence. Direct calls derive grids from declared schemas.
 * No payload is read. Coordinate conversion and index storage charge root.
 */
inline Result<TileNeed> tile_need_for_samples(const ResourceBudget& root,
                                              const ResultProgramQuery& query,
                                              std::uint32_t input,
                                              std::uint32_t slot,
                                              const Footprint& samples,
                                              bool descriptor_only = false) {
  using Answer = Result<TileNeed>;
  if (input >= query.inputs.size() || !query.inputs[input].result_schema)
    return Answer(
        Status{ErrorCode::InvalidArgument, "invalid tile Need input"});
  const auto& schema = *query.inputs[input].result_schema;
  if (slot >= schema.tensors.size() || !samples.valid() ||
      samples.shape() != schema.tensors[slot].sample_shape())
    return Answer(
        Status{ErrorCode::InvalidArgument, "invalid tile Need domain"});
  if (descriptor_only)
    return Answer(TileNeed{input, false, true, {}});
  if (samples.empty())
    return Answer(
        Status{ErrorCode::InvalidArgument, "empty tile payload Need"});
  std::optional<TileGrid> grid;
  if (query.input_tile_grids.empty()) {
    auto declared = TileGrid::from_schema(schema, query.tile_extent);
    if (!declared.ok())
      return Answer(declared.status());
    grid = declared.take_value();
  } else {
    if (query.input_tile_grids.size() != query.inputs.size())
      return Answer(
          Status{ErrorCode::InvalidArgument, "incomplete input grids"});
    grid = query.input_tile_grids[input];
  }
  if (!grid)
    return Answer(TileNeed{input, true, false, {}});
  auto tiles = tiles_for_samples(root, *grid, samples);
  if (!tiles.ok())
    return Answer(tiles.status());
  return Answer(TileNeed{input, false, false, tiles.take_value()});
}
}  // namespace ps::sdk
