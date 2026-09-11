#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "photospider/data/value_fragments.hpp"

namespace ps {
/** @brief Fixed slot layout consumed by CPU and GPU lookup implementations.
 * @note Each slot is 80 little-endian bytes: eight uint64 tile coordinates,
 * uint64 valid mask, uint64 payload byte offset. A zero mask denotes an empty
 * slot. Payload contains only valid samples, ordered by increasing mask bit.
 * Tile volume is at most 64; unused coordinate fields are zero. Lookups hash
 * rank coordinate words using FNV-1a bytes (low byte first), then linear probe
 * a power-of-two directory. Physical binding needs only payload and directory.
 */
inline constexpr std::uint64_t kFragmentAtlasSlotBytes = 80;
/** @brief Immutable packed samples and lookup directory; no source owners.
 * @note Lookup applies to global descriptor coordinates. Boundary mapping is
 * the sampling algorithm's responsibility and must precede lookup. A hole is
 * NotFound; it is never a zero sample or a per-fragment clamp.
 */
struct PHOTOSPIDER_API FragmentAtlas final {
  ValueDescriptor descriptor;
  std::vector<std::uint64_t> tile_shape;
  std::uint64_t slot_count = 0, payload_bytes = 0;
  Value payload, directory;
  /** @brief Exact byte offset of a valid sample in payload, or a typed failure.
   */
  Result<std::uint64_t> address(
      const std::vector<std::uint64_t>& coordinate) const;
};
/** @brief Immutable transport plan with exact allocation sizes and no pixels.
 * @note Prepare and materialize are separately bounded. The plan stores only
 * domain/coverage and occupied tiles, never source Value or allocator owners.
 * GPU hosts can round the two exact payload sizes to their device allocation
 * capacities before admitting materialization. Copies share immutable metadata.
 */
class PHOTOSPIDER_API FragmentAtlasPlan final {
 public:
  FragmentAtlasPlan() = default;
  /** @brief Builds a bounded tile directory from exact authorized samples.
   * @param tile_shape Positive rank-sized extents with product <=64. Empty
   * chooses a deterministic geometry with at most 64 samples per tile.
   * @param limits maximum_boxes bounds occupied tiles; maximum_work bounds
   * sample discovery and directory probes before allocation. Cancellation is
   * checked during traversal. No bounding-box samples are added.
   */
  static Result<FragmentAtlasPlan> prepare(
      const ValueFragments& input, std::vector<std::uint64_t> tile_shape = {},
      const FootprintLimits& limits = {});
  bool valid() const noexcept { return impl_ != nullptr; }
  /** @brief Required physical payload sizes; Empty uses one inert payload byte
   * and a two-slot empty directory. These are not authorized source samples.
   * @throws std::logic_error If this plan is invalid.
   */
  std::uint64_t payload_allocation_bytes() const;
  std::uint64_t directory_allocation_bytes() const;
  std::uint64_t occupied_tiles() const;
  /** @brief Charged preparation and required matching-input packing work.
   * @note Hosts use these conservative units for an enclosing Run budget.
   * @throws std::logic_error If this plan is invalid.
   */
  std::uint64_t preparation_work() const;
  std::uint64_t materialization_work() const;
  /** @brief Copies current input samples into the admitted allocator.
   * @note Input dtype/domain/coverage must match the plan. Actual bits are read
   * now, preserving signed strides and fragment authorization. Input values and
   * facets are not retained. Limits bound packing work; errors publish nothing.
   */
  Result<FragmentAtlas> materialize(const ValueFragments& input,
                                    const BufferAllocator& allocator,
                                    const FootprintLimits& limits = {}) const;

 private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
};
}  // namespace ps
