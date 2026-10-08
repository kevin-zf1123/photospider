#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "photospider/data/value_fragments.hpp"

namespace ps {
class ResultTensorInput;
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
  /** @brief Builds a plan from a Result tensor capability's sample coverage.
   * @details This overload uses the capability's schema and coverage but does
   * not read source samples or retain the source Result. It shares the generic
   * directory-planning algorithm and work bounds. A descriptor-only capability
   * can describe an empty plan, but materialization still requires a
   * payload-authorized Need. An Empty payload Need produces a valid empty plan.
   * @param tile_shape Positive rank-sized extents with product <=64. Empty
   * chooses a deterministic geometry with at most 64 samples per tile.
   * @param limits Bounds tile discovery and probes; consume_work precharges
   * work before the corresponding traversal steps. Cancellation is checked.
   */
  static Result<FragmentAtlasPlan> prepare(
      const ResultTensorInput& input,
      std::vector<std::uint64_t> tile_shape = {},
      const FootprintLimits& limits = {});
  bool valid() const noexcept { return impl_ != nullptr; }
  /** @brief Required physical payload sizes; Empty uses one inert payload byte
   * and a two-slot empty directory. These are not authorized source samples.
   * @throws std::logic_error If this plan is invalid.
   */
  std::uint64_t payload_allocation_bytes() const;
  std::uint64_t directory_allocation_bytes() const;
  std::uint64_t occupied_tiles() const;
  /** @brief Reports generic geometry-planning work.
   * @note For the ValueFragments overload, callers retain their existing
   * convention of charging this report externally. The ResultTensorInput
   * prepare overload also calls FootprintLimits::consume_work directly.
   * @throws std::logic_error If this plan is invalid.
   */
  std::uint64_t preparation_work() const;
  /** @brief Reports generic packing work for the matching input coverage.
   * @note The ResultTensorInput materialize overload additionally charges
   * window acquisition and read-bound work through consume_work; this report
   * is not its complete charge. ValueFragments keeps its caller-charged work
   * convention.
   * @throws std::logic_error If this plan is invalid.
   */
  std::uint64_t materialization_work() const;
  /** @brief Copies current input samples into the admitted allocator.
   * @note Input dtype/domain/coverage must match the plan. Actual bits are read
   * now, preserving signed strides and fragment authorization. Input values and
   * facets are not retained. Limits bound packing work; errors publish nothing.
   */
  Result<FragmentAtlas> materialize(const ValueFragments& input,
                                    const BufferAllocator& allocator,
                                    const FootprintLimits& limits = {}) const;
  /** @brief Packs the exact samples authorized by a Result tensor Need.
   * @details Input type, sample shape, and coverage must match this plan. The
   * method acquires owning windows for those samples and uses their row runs,
   * including signed or broadcast strides and fragmented physical windows.
   * It calls limits.consume_work directly for window lookup bounds and packing
   * work. An unauthorized descriptor-only capability fails with the sticky
   * InvalidArgument read-protocol status. An Empty payload Need returns an
   * atlas with one inert payload byte and a two-slot empty directory without
   * reading source samples.
   * The returned atlas owns packed payload and directory storage but retains
   * no source Result or read window.
   * @param input Need-authorized tensor capability matching this plan.
   * @param allocator Owns the atlas's packed buffers.
   * @param limits Bounds sample reads and packing; consume_work precharges
   * each bounded phase before its work. Errors publish no atlas.
   */
  Result<FragmentAtlas> materialize(const ResultTensorInput& input,
                                    const BufferAllocator& allocator,
                                    const FootprintLimits& limits = {}) const;

 private:
  static Result<FragmentAtlasPlan> prepare_regions(
      const ValueDescriptor& descriptor, const Footprint& coverage,
      std::vector<std::uint64_t> geometry, const FootprintLimits& limits);
  Result<FragmentAtlas> materialize_regions(
      const ValueDescriptor& descriptor, const Footprint& coverage,
      const std::function<Status(const std::vector<std::uint64_t>&, void*,
                                 std::size_t)>& read,
      const BufferAllocator& allocator, const FootprintLimits& limits) const;
  struct Impl;
  std::shared_ptr<const Impl> impl_;
};
}  // namespace ps
