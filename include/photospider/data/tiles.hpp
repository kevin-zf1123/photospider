#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "photospider/core/resource_allocator.hpp"
#include "photospider/data/region.hpp"

namespace ps {
struct SchemaTemplate;
using TileIndex = std::uint64_t;

/** @brief Immutable sorted unique producer-unit indices.
 * @note Construction, union and copying through copy_to charge the supplied
 * Root's work and metadata. Copies share immutable storage and its Root owner.
 * A default set is empty. No dense producer-domain allocation is made.
 */
class PHOTOSPIDER_API TileSet final {
 public:
  TileSet() = default;
  static Result<TileSet> from_indices(const ResourceBudget& root,
                                      const std::vector<TileIndex>& indices);
  /** @brief Copies borrowed indices into sorted, Root-owned storage.
   * A null pointer is valid only when count is zero. No owner is borrowed.
   */
  static Result<TileSet> from_indices(const ResourceBudget& root,
                                      const TileIndex* indices,
                                      std::size_t count);
  Result<TileSet> unite(const TileSet& other, const ResourceBudget& root) const;
  Result<TileSet> copy_to(const ResourceBudget& root) const;
  bool empty() const noexcept { return size() == 0; }
  std::size_t size() const noexcept;
  /** @brief Whether nonempty index storage belongs to the supplied Root.
   * Empty sets carry no allocation and are valid for any Root.
   */
  bool owned_by(const ResourceBudget& root) const noexcept;
  bool contains(TileIndex index) const noexcept;
  const TileIndex* begin() const noexcept;
  const TileIndex* end() const noexcept;
  bool operator==(const TileSet& other) const noexcept;
  bool operator!=(const TileSet& other) const noexcept {
    return !(*this == other);
  }

 private:
  struct Storage;
  std::shared_ptr<const Storage> storage_;
};

/** @brief Compiled square spatial grid, independent of physical storage.
 * @note Batch coordinates precede cell axes. Every non-spatial cell axis is
 * complete in each tile. Grid creation rejects tile-count overflow without
 * requiring the tensor's full sample count to fit uint64_t.
 */
class PHOTOSPIDER_API TileGrid final {
 public:
  static Result<std::optional<TileGrid>> from_schema(
      const SchemaTemplate& schema, std::uint64_t tile_extent = 128);
  std::uint64_t tile_extent() const noexcept { return extent_; }
  std::uint64_t batch_count() const noexcept { return batches_; }
  std::uint64_t tile_rows() const noexcept { return rows_; }
  std::uint64_t tile_columns() const noexcept { return columns_; }
  TileIndex tile_count() const noexcept { return count_; }
  std::uint32_t batch_rank() const noexcept { return batch_rank_; }
  std::uint32_t rank() const noexcept { return rank_; }
  std::uint32_t height_axis() const noexcept { return height_axis_; }
  std::uint32_t width_axis() const noexcept { return width_axis_; }
  const std::array<std::uint64_t, 8>& shape() const noexcept { return shape_; }
  /** @brief Resolves one tile index to its clipped logical sample region.
   * @return InvalidArgument for an out-of-grid index. Does not allocate pixels.
   * @throws std::bad_alloc For the bounded rank-at-most-eight Region metadata.
   */
  Result<Region> region(TileIndex tile) const;

 private:
  std::uint64_t extent_ = 128, batches_ = 0, rows_ = 0, columns_ = 0;
  TileIndex count_ = 0;
  std::uint32_t rank_ = 0, batch_rank_ = 0, height_axis_ = 0, width_axis_ = 0;
  std::array<std::uint64_t, 8> shape_{};
};

/** @brief One caller request: complete producer or an explicit tile set.
 * An empty set with whole=false is an empty request, never an implicit Whole.
 */
struct PHOTOSPIDER_API TileRequest final {
  bool whole = false;
  TileSet tiles;
  /** @brief Checks flags and bounds, then resolves nonempty Whole producers.
   * Empty remains empty. Nonempty indices require a grid even for Whole
   * producers. Validation reads the largest sorted index without allocation.
   * Returned sets share their immutable Root-owned storage.
   */
  Result<TileRequest> resolve(const std::optional<TileGrid>& grid,
                              bool tiled) const;
  /** @brief Whether this normalized request includes another request.
   * Both requests must have been resolved against the same producer grid.
   * Work is linear in the two sparse sets; no dense grid is enumerated.
   */
  bool contains(const TileRequest& other) const noexcept;
  bool operator==(const TileRequest& other) const noexcept {
    return whole == other.whole && tiles == other.tiles;
  }
  bool operator!=(const TileRequest& other) const noexcept {
    return !(*this == other);
  }
};
/** @brief One input request; exactly one of tiles, Whole or descriptor only.
 * Whole and descriptor-only requests require an empty tiles set. Tile indices
 * address the producer's grid, including when its computation is Whole.
 */
struct PHOTOSPIDER_API TileNeed final {
  std::uint32_t input = 0;
  bool whole = false, descriptor_only = false;
  TileSet tiles;
  Status validate() const;
  /** @brief Checks grid bounds and normalizes spatial Whole producers.
   * The grid and granularity come from the compiled producer, never storage.
   * Scanning indices charges root work. Returned immutable sets share owners.
   */
  Result<TileNeed> resolve(const std::optional<TileGrid>& grid, bool tiled,
                           const ResourceBudget& root) const;
};
}  // namespace ps
