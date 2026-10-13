#pragma once

#include <cstddef>
#include <cstdint>
#include <tuple>

#include "photospider/compiler/compiler.hpp"
#include "photospider/data/tiles.hpp"

namespace ps {
/** @brief One computation unit of a compiled selected output.
 * Whole outputs always use tile zero. Object identity and publication progress
 * are excluded. The containing plan supplies physical and semantic identity.
 */
struct TileUnit final {
  std::size_t step = 0;
  std::uint32_t output = 0;
  TileIndex tile = 0;
  bool operator<(const TileUnit& other) const noexcept {
    return std::tie(step, output, tile) <
           std::tie(other.step, other.output, other.tile);
  }
  bool operator==(const TileUnit& other) const noexcept {
    return step == other.step && output == other.output && tile == other.tile;
  }
};
/** @brief Complete producer, explicit producer tiles, or descriptor only.
 * Empty tiles with whole=false means the semantic descriptor only. Every
 * dependency also observes semantic descriptor changes. A Whole producer's
 * payload dependency is represented by whole=true, never storage tile keys.
 */
struct TileDependency final {
  PlanInput producer;
  std::uint32_t producer_output = 0;
  bool whole = false;
  TileSet tiles;
};
/** @brief Union of all admitted Needs for one completed computation unit.
 * Copies retain immutable TileSet owners; input-vector copies use its Root.
 */
struct PHOTOSPIDER_API TileRecord final {
  explicit TileRecord(const ResourceBudget& root)
      : inputs(ResourceAllocator<TileDependency>(root)) {}
  TileUnit unit;
  /** @brief This unit determines runtime global descriptor facts.
   * Derived from the compiled output schema. When the unit becomes dirty,
   * descriptor-only consumers may change even without requesting its payload.
   */
  bool computed_descriptor = false;
  ResourceVector<TileDependency> inputs;
  /** @brief Adds a normalized dependency, merging repeated producer Needs.
   * Whole dominates tile/descriptor requests. New index storage, copying and
   * scanning charge root; failure leaves this record unchanged.
   */
  Status include(const TileDependency& dependency, const ResourceBudget& root);
};
/** @brief Changed logical content units and/or semantic descriptor of a source.
 * Coverage, revision, sealed state and object identity are not semantic edits.
 */
struct TileChange final {
  PlanInput producer;
  std::uint32_t producer_output = 0;
  TileSet tiles;
  bool descriptor_changed = false;
};
}  // namespace ps
