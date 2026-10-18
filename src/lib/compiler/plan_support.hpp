#pragma once
#include <tuple>
#include <utility>
#include <vector>

#include "compiler/limits.hpp"
#include "photospider/compiler/plan.hpp"

namespace ps::compiler_internal::plan_support {
inline Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument, message};
}
using Unit = std::tuple<std::size_t, std::uint32_t, std::uint32_t, TileIndex>;

inline Result<Footprint> coverage(const TileGrid& grid, const TileSet& tiles) {
  return grid.coverage(tiles, compiler_internal::static_set_limits());
}

inline Result<std::vector<Footprint>> coverage(
    const std::vector<TileGrid>& grids, const TileRequest& request) {
  std::vector<Footprint> result;
  for (std::uint32_t i = 0; i < grids.size(); ++i) {
    auto found = request.members.find(i);
    auto samples = coverage(
        grids[i], found == request.members.end() ? TileSet{} : found->second);
    if (!samples.ok())
      return Result<std::vector<Footprint>>(samples.status());
    result.push_back(samples.take_value());
  }
  return Result<std::vector<Footprint>>(std::move(result));
}
inline Result<TileRequest> close_publication(const SchemaTemplate& schema,
                                             const std::vector<TileGrid>& grids,
                                             TileRequest request) {
  auto resolved = request.resolve(grids);
  if (!resolved.ok())
    return resolved;
  request = resolved.take_value();
  for (const auto& scope : schema.publication_scopes) {
    bool used = false;
    for (auto member : scope.members)
      used = used || request.members.count(member);
    if (!used)
      continue;
    if (scope.policy == ResultPublicationPolicy::CompleteBundle) {
      for (auto member : scope.members) {
        auto all =
            TileSet::range(ResourceBudget{}, 0, grids[member].tile_count());
        if (!all.ok())
          return Result<TileRequest>(all.status());
        request.members[member] = all.take_value();
      }
    } else if (scope.policy == ResultPublicationPolicy::StablePrefix) {
      auto member = scope.members[0];
      const auto& selected = request.members.at(member);
      if (!selected.empty()) {
        auto prefix = TileSet::range(ResourceBudget{}, 0,
                                     selected.intervals().back().end);
        if (!prefix.ok())
          return Result<TileRequest>(prefix.status());
        request.members[member] = prefix.take_value();
      }
    }
  }
  return Result<TileRequest>(std::move(request));
}
// Validators are compiler-visible dependencies: their full certification
// barrier is resolved before any consumer can observe the selected value.
inline Result<TileRequest> certification_closure(const PlanStep& step,
                                                 std::uint32_t stage,
                                                 TileRequest request) {
  const bool complete =
      stage == UINT32_MAX
          ? step.prepared->requires_complete_validation(true, step.output_index)
          : step.prepared->requires_complete_stage_validation(stage);
  if (complete && !request.empty())
    return TileRequest::all(step.tile_grids, ResourceBudget{});
  return close_publication(*step.output_result_schema, step.tile_grids,
                           std::move(request));
}
}  // namespace ps::compiler_internal::plan_support
