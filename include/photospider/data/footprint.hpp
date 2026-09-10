#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "photospider/data/region.hpp"
#include "photospider/execution/cancellation.hpp"

namespace ps {

/** @brief Independent exact-set construction limits; no bounding-box fallback.
 * @note Work counts candidates, including duplicates. Limits apply to each
 * operation, not process RSS. Cancellation is observed during construction.
 */
struct FootprintLimits final {
  std::uint64_t maximum_boxes = 65536;
  std::uint64_t maximum_work = 1048576;
  CancellationToken cancellation;
};

/** @brief Immutable exact logical set in a nonzero rank-1..8 domain.
 * @note Canonical disjoint rectangles represent Empty, All and sparse runs.
 * Construction never enumerates samples or multiplies the domain extents.
 * Equal sets in the same domain have identical boxes, independent of insertion
 * order/tiling. Copies own metadata; concurrent const access is safe.
 * All allocating methods may throw bad_alloc. Limit failures return
 * ResourceExhausted, cancellation returns Cancelled, and domain mismatches
 * return InvalidArgument. Failed operations never publish partial coverage.
 */
class PHOTOSPIDER_API Footprint final {
 public:
  /** @brief Default is an invalid domain, distinct from an exact empty set. */
  Footprint() = default;
  /** @brief Canonicalizes checked rectangles, retaining holes exactly.
   * @param shape Nonzero extents, rank 1..8; dense product may overflow.
   * @param boxes Rectangles in this domain; empty rectangles contribute
   * nothing.
   * @param limits Bounds checked before intermediate set growth.
   */
  static Result<Footprint> from_regions(std::vector<std::uint64_t> shape,
                                        const std::vector<Region>& boxes,
                                        const FootprintLimits& limits = {});
  /** @brief Creates All without enumerating samples. */
  static Result<Footprint> all(std::vector<std::uint64_t> shape,
                               const FootprintLimits& limits = {});
  /** @brief Creates a known empty set in the supplied domain. */
  static Result<Footprint> none(std::vector<std::uint64_t> shape,
                                const FootprintLimits& limits = {});
  bool valid() const noexcept { return !shape_.empty(); }
  bool empty() const noexcept { return boxes_.empty(); }
  /** @brief Borrowed domain and canonical boxes, valid until move/destruction.
   */
  const std::vector<std::uint64_t>& shape() const noexcept { return shape_; }
  const std::vector<Region>& boxes() const noexcept { return boxes_; }
  /** @brief Exact coordinate membership; false for malformed/outside points.
   */
  bool contains(const std::vector<std::uint64_t>& coordinate) const noexcept;
  /** @brief Exact set operations on equal domains. */
  Result<Footprint> unite(const Footprint& other,
                          const FootprintLimits& limits = {}) const;
  Result<Footprint> intersect(const Footprint& other,
                              const FootprintLimits& limits = {}) const;
  Result<Footprint> subtract(const Footprint& other,
                             const FootprintLimits& limits = {}) const;
  bool operator==(const Footprint& other) const noexcept;
  bool operator!=(const Footprint& other) const noexcept {
    return !(*this == other);
  }
  /** @brief Checked cardinality; All may legitimately exceed uint64. */
  Result<std::uint64_t> element_count() const;
  /** @brief Exact touched tile coordinates in a ceil-divided tile domain.
   * @param geometry Positive tile extents in descriptor axis order.
   * @note Returned tiles do not assert full sample validity inside a tile.
   */
  Result<Footprint> tile_cover(const std::vector<std::uint64_t>& geometry,
                               const FootprintLimits& limits = {}) const;
  /** @brief Visits each sample once in canonical logical row-major order.
   * @param visitor Borrowed callback; false/error interrupts immediately.
   * @param maximum_samples Work bound checked before each callback.
   * @param cancellation Observed before each callback, including empty queries.
   * @return First callback failure, Cancelled, ResourceExhausted or success.
   * @note Does not allocate a dense coordinate list. Callback exceptions
   * propagate; already delivered observations cannot be rolled back.
   */
  Status visit(
      const std::function<Status(const std::vector<std::uint64_t>&)>& visitor,
      std::uint64_t maximum_samples,
      const CancellationToken& cancellation = {}) const;

 private:
  std::vector<std::uint64_t> shape_;
  std::vector<Region> boxes_;
};
}  // namespace ps
