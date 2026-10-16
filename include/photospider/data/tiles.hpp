#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "photospider/core/resource_allocator.hpp"
#include "photospider/data/footprint.hpp"
#include "photospider/data/region.hpp"

namespace ps {
struct SchemaTemplate;
struct ResultTensorSpec;
using TileIndex = std::uint64_t;

/** @brief Half-open interval of local tile indices [begin,end). */
struct TileInterval final {
  TileIndex begin = 0, end = 0;
  bool operator==(const TileInterval& other) const noexcept {
    return begin == other.begin && end == other.end;
  }
};

/** @brief Immutable normalized intervals of producer-unit indices.
 * @note Construction, union and copying through copy_to charge the supplied
 * Root's work and metadata. Copies share immutable storage and its Root owner.
 * A default set is empty. No dense producer-domain allocation is made.
 */
class PHOTOSPIDER_API TileSet final {
 public:
  TileSet() = default;
  /** @brief Normalizes intervals without enumerating their member indices.
   * Empty intervals are ignored; reversed intervals are invalid. Adjacent and
   * overlapping intervals coalesce. Work and storage scale with interval count.
   */
  static Result<TileSet> from_intervals(const ResourceBudget& root,
                                        const TileInterval* intervals,
                                        std::size_t count);
  static Result<TileSet> range(const ResourceBudget& root, TileIndex begin,
                               TileIndex end);
  const ResourceVector<TileInterval>& intervals() const noexcept;
  bool contains(const TileSet& other) const noexcept;
  bool intersects(const TileSet& other) const noexcept;
  /** @brief Number of represented indices; allocation uses interval count. */
  TileIndex at(std::uint64_t position) const noexcept;
  class Iterator final {
   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = TileIndex;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = TileIndex;
    Iterator() = default;
    Iterator(const TileSet* owner, std::uint64_t position)
        : owner_(owner), position_(position) {}
    TileIndex operator*() const { return owner_->at(position_); }
    TileIndex operator[](std::uint64_t distance) const {
      return owner_->at(position_ + distance);
    }
    Iterator& operator++() {
      ++position_;
      return *this;
    }
    Iterator operator++(int) {
      auto old = *this;
      ++*this;
      return old;
    }
    Iterator& operator--() {
      --position_;
      return *this;
    }
    Iterator operator--(int) {
      auto old = *this;
      --*this;
      return old;
    }
    Iterator operator-(std::uint64_t distance) const {
      return {owner_, position_ - distance};
    }
    bool operator==(const Iterator& other) const {
      return owner_ == other.owner_ && position_ == other.position_;
    }
    bool operator!=(const Iterator& other) const { return !(*this == other); }

   private:
    const TileSet* owner_ = nullptr;
    std::uint64_t position_ = 0;
  };
  static Result<TileSet> from_indices(const ResourceBudget& root,
                                      const std::vector<TileIndex>& indices);
  /** @brief Copies borrowed indices into sorted, Root-owned storage.
   * A null pointer is valid only when count is zero. No owner is borrowed.
   */
  static Result<TileSet> from_indices(const ResourceBudget& root,
                                      const TileIndex* indices,
                                      std::size_t count);
  Result<TileSet> unite(const TileSet& other, const ResourceBudget& root) const;
  Result<TileSet> intersect(const TileSet& other,
                            const ResourceBudget& root) const;
  Result<TileSet> subtract(const TileSet& other,
                           const ResourceBudget& root) const;
  Result<TileSet> copy_to(const ResourceBudget& root) const;
  bool empty() const noexcept { return size() == 0; }
  std::size_t size() const noexcept;
  /** @brief Whether nonempty index storage belongs to the supplied Root.
   * Empty sets carry no allocation and are valid for any Root.
   */
  bool owned_by(const ResourceBudget& root) const noexcept;
  bool contains(TileIndex index) const noexcept;
  Iterator begin() const noexcept { return {this, 0}; }
  Iterator end() const noexcept { return {this, size()}; }
  bool operator==(const TileSet& other) const noexcept;
  bool operator!=(const TileSet& other) const noexcept {
    return !(*this == other);
  }

 private:
  struct Storage;
  std::shared_ptr<const Storage> storage_;
};

/** @brief Compiled member grid, independent of physical storage.
 * @note Spatial members tile H/W and cover other cell axes completely.
 * Non-spatial members tile their last two free cell axes, with earlier free
 * axes acting as batch coordinates. Atomic trailing axes remain complete. Grid
 * creation rejects tile-count overflow without requiring the tensor's full
 * sample count to fit uint64_t.
 */
class PHOTOSPIDER_API TileGrid final {
 public:
  static Result<std::optional<TileGrid>> from_schema(
      const SchemaTemplate& schema, std::uint64_t tile_extent = 128,
      std::uint32_t tensor_slot = 0);
  static Result<std::vector<TileGrid>> members(const SchemaTemplate& schema,
                                               std::uint64_t tile_extent = 128);
  static Result<TileGrid> from_tensor(const ResultTensorSpec& tensor,
                                      std::uint64_t tile_extent = 128);
  std::uint64_t tile_extent() const noexcept { return extent_; }
  std::uint64_t batch_count() const noexcept { return batches_; }
  std::uint64_t tile_rows() const noexcept { return rows_; }
  std::uint64_t tile_columns() const noexcept { return columns_; }
  TileIndex tile_count() const noexcept { return count_; }
  std::uint32_t batch_rank() const noexcept { return batch_rank_; }
  std::uint32_t rank() const noexcept { return rank_; }
  std::uint32_t batch_axis_mask() const noexcept { return batch_axis_mask_; }
  std::uint32_t height_axis() const noexcept { return height_axis_; }
  std::uint32_t width_axis() const noexcept { return width_axis_; }
  const std::array<std::uint64_t, 8>& shape() const noexcept { return shape_; }
  /** @brief Resolves one tile index to its clipped logical sample region.
   * @return InvalidArgument for an out-of-grid index. Does not allocate pixels.
   * @throws std::bad_alloc For the bounded rank-at-most-eight Region metadata.
   */
  Result<Region> region(TileIndex tile) const;
  /** @brief Converts intervals to complete tile coverage without visiting each
   * tile. Aligned row/batch spans become rectangles; work scales with interval
   * boundaries and rank, subject to Footprint normalization limits.
   */
  Result<Footprint> coverage(const TileSet& tiles,
                             const FootprintLimits& limits = {}) const;

 private:
  std::uint64_t extent_ = 128, batches_ = 0, rows_ = 0, columns_ = 0;
  TileIndex count_ = 0;
  std::uint32_t rank_ = 0, batch_rank_ = 0, height_axis_ = 0, width_axis_ = 0;
  std::uint32_t batch_axis_mask_ = 0;
  std::array<std::uint64_t, 8> shape_{};
};

/** @brief Address of one member-local computation unit. */
struct TileAddress final {
  std::uint32_t tensor_slot = 0;
  TileIndex tile = 0;
};
/** @brief Explicit member selections. An empty map requests no payload.
 * All-member acquisition is constructed from static grids as ordinary ranges.
 * The map contains at most one interval set per member; empty sets are omitted
 * by resolve(). No scheduling or storage policy is encoded in this request.
 */
struct PHOTOSPIDER_API TileRequest final {
  std::map<std::uint32_t, TileSet> members;
  static Result<TileRequest> all(const std::vector<TileGrid>& grids,
                                 const ResourceBudget& root);
  Result<TileRequest> resolve(const std::vector<TileGrid>& grids) const;
  Result<TileRequest> copy_to(const ResourceBudget& root) const;
  Result<TileRequest> unite(const TileRequest& other,
                            const ResourceBudget& root) const;
  bool empty() const noexcept { return members.empty(); }
  std::uint64_t size() const noexcept;
  std::size_t interval_count() const noexcept;
  TileAddress at(std::uint64_t position) const noexcept;
  bool contains(const TileRequest& other) const noexcept;
  bool operator==(const TileRequest& other) const noexcept {
    return members == other.members;
  }
  bool operator!=(const TileRequest& other) const noexcept {
    return !(*this == other);
  }
};
/** @brief One member's payload intervals or an input's static descriptor.
 * Descriptor requests carry no tiles and tensor_slot must be zero. The host
 * validates the complete Need envelope before scheduling any producer work.
 */
struct PHOTOSPIDER_API TileNeed final {
  std::uint32_t input = 0, tensor_slot = 0;
  bool descriptor_only = false;
  TileSet tiles;
  Status validate() const;
  Result<TileNeed> resolve(const std::vector<TileGrid>& grids,
                           const ResourceBudget& root) const;
};
}  // namespace ps
