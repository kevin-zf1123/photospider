#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <tuple>

#include "photospider/compiler/compiler.hpp"
#include "photospider/data/tiles.hpp"

namespace ps {
/** @brief One computation unit of a compiled selected output.
 * Member grids define tile indices. Object identity and publication progress
 * are excluded. The containing plan supplies physical and semantic identity.
 */
struct TileUnit final {
  std::size_t step = 0;
  std::uint32_t output = 0, tensor_slot = 0;
  TileIndex tile = 0;
  bool operator<(const TileUnit& other) const noexcept {
    return std::tie(step, output, tensor_slot, tile) <
           std::tie(other.step, other.output, other.tensor_slot, other.tile);
  }
  bool operator==(const TileUnit& other) const noexcept {
    return step == other.step && output == other.output &&
           tensor_slot == other.tensor_slot && tile == other.tile;
  }
};
/** @brief Explicit producer member tile intervals, or descriptor-only access.
 * Empty intervals observe only static descriptor facts. Every payload
 * dependency also observes descriptor changes. Storage partitions never
 * identify units.
 */
struct TileDependency final {
  PlanInput producer;
  std::uint32_t producer_output = 0;
  std::uint32_t tensor_slot = 0;
  TileSet tiles;
};
/** @brief Immutable shared dependency evidence; never a read capability.
 * Each node owns only its local Needs and references prior frozen evidence.
 * The execution table indexes each shared node once, independently of the
 * number of consuming tiles. Nodes and interval owners use the Run Root.
 */
struct TileEvidence final {
  explicit TileEvidence(const ResourceBudget& root)
      : inputs(ResourceAllocator<TileDependency>(root)),
        inherited(
            ResourceAllocator<std::shared_ptr<const TileEvidence>>(root)) {
    static std::atomic<std::uint64_t> next{1};
    identity = next.load(std::memory_order_relaxed);
    do {
      if (identity == UINT64_MAX)
        throw std::bad_alloc();
    } while (!next.compare_exchange_weak(identity, identity + 1,
                                         std::memory_order_relaxed));
  }
  std::uint64_t identity = 0;
  TileUnit owner;
  ResourceVector<TileDependency> inputs;
  ResourceVector<std::shared_ptr<const TileEvidence>> inherited;
};
/** @brief Union of all admitted Needs for one completed computation unit.
 * Copies retain immutable TileSet owners; input-vector copies use its Root.
 */
struct PHOTOSPIDER_API TileRecord final {
  explicit TileRecord(const ResourceBudget& root)
      : inputs(ResourceAllocator<TileDependency>(root)),
        shared(ResourceAllocator<std::shared_ptr<const TileEvidence>>(root)) {}
  TileUnit unit;
  ResourceVector<TileDependency> inputs;
  ResourceVector<std::shared_ptr<const TileEvidence>> shared;
  Status inherit(std::shared_ptr<const TileEvidence> evidence,
                 const ResourceBudget& root);
  Result<std::shared_ptr<const TileEvidence>> freeze(
      const ResourceBudget& root) const;
  /** @brief Adds a normalized dependency, merging repeated producer Needs.
   * Intervals merge only within one producer/member. New interval storage,
   * copying and scanning charge root; failure leaves this record unchanged.
   */
  Status include(const TileDependency& dependency, const ResourceBudget& root);
};
/** @brief Changed logical content units and/or semantic descriptor of a source.
 * Coverage, revision, sealed state and object identity are not semantic edits.
 */
struct TileChange final {
  PlanInput producer;
  std::uint32_t producer_output = 0, tensor_slot = 0;
  TileSet tiles;
  bool descriptor_changed = false;
};
}  // namespace ps
