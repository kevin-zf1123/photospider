#pragma once

#include <map>
#include <utility>
#include <vector>

#include "photospider/compiler/physical_program.hpp"
#include "photospider/data/tiles.hpp"

namespace ps::program_internal {
/** @brief Collect the normalized member tile sets of a validated physical
 * value. */
inline Result<TileRequest> request_from_value(const physical::Program& p,
                                              physical::Id vi) {
  TileRequest result;
  std::map<std::uint32_t, std::vector<TileIndex>> members;
  for (auto tile : p.values[vi].tiles)
    members[p.tiles[tile].member].push_back(p.tiles[tile].tile_index);
  for (const auto& member : members) {
    auto set = TileSet::from_indices(ResourceBudget{}, member.second);
    if (!set.ok())
      return Result<TileRequest>(set.status());
    result.members.emplace(member.first, set.take_value());
  }
  return Result<TileRequest>(std::move(result));
}

}  // namespace ps::program_internal
