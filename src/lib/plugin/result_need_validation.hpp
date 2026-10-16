#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"
#include "photospider/plugin/result_program.hpp"

namespace ps::plugin_internal {
inline Status invalid_result_need(const char* message) {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::None,
          {FailureOrigin::Protocol, FailureScope::Group}};
}
inline Status validate_result_need(
    const ResultProgramNeed& need, const ResultProgramQuery& query,
    const OperationOutputTraits& output, const ResourceBudget& resources,
    std::uint64_t maximum, bool allow_empty = false,
    const std::function<Status(std::uint64_t)>& consume_work = {}) {
  const auto count = need.tiles.size() + need.shared_stages.size();
  if ((count == 0 && !allow_empty) || count > maximum ||
      (!need.tiles.empty() &&
       !need.tiles.get_allocator().owned_by(resources)) ||
      (!need.shared_stages.empty() &&
       !need.shared_stages.get_allocator().owned_by(resources)))
    return invalid_result_need("invalid structured Need envelope");
  if (consume_work) {
    auto charged = consume_work(count);
    if (!charged.ok())
      return charged;
  }
  if (query.inputs.size() > 1024)
    return invalid_result_need("invalid Result Need input domain");
  const auto selected_port = [&](std::uint32_t port) {
    const auto& selection = output.input_indices;
    return !selection || std::find(selection->begin(), selection->end(),
                                   port) != selection->end();
  };
  if (!need.shared_stages.empty()) {
    if (!query.prepared)
      return invalid_result_need("shared stage requires static preparation");
    for (std::size_t i = 0; i < need.shared_stages.size(); ++i) {
      const auto stage = need.shared_stages[i].stage;
      if (stage >= query.prepared->traits().shared_stages.size())
        return invalid_result_need("unknown shared stage");
      if (need.shared_stages[i].tiles) {
        auto grids = TileGrid::members(
            query.prepared->traits().shared_stages[stage].result_schema,
            query.tile_extent);
        if (!grids.ok())
          return grids.status();
        for (const auto& member : need.shared_stages[i].tiles->members)
          if (!member.second.owned_by(resources))
            return invalid_result_need(
                "shared tile indices belong to another Root");
        auto resolved = need.shared_stages[i].tiles->resolve(grids.value());
        if (!resolved.ok())
          return resolved.status();
      }
      for (auto port :
           query.prepared->traits().shared_stages[stage].input_indices)
        if (!selected_port(port))
          return invalid_result_need("shared stage exceeds input projection");
      for (std::size_t j = 0; j < i; ++j)
        if (need.shared_stages[j].stage == stage)
          return invalid_result_need("duplicate shared stage Need");
    }
  }
  // Validate the complete envelope before executing any producer.
  for (const auto& input : need.tiles) {
    if (input.input >= query.inputs.size() || !selected_port(input.input) ||
        !query.inputs[input.input].result_schema)
      return invalid_result_need("invalid tile Need input");
    if (!input.tiles.owned_by(resources))
      return invalid_result_need("tile Need indices belong to another Root");
    auto flags = input.validate();
    if (!flags.ok())
      return invalid_result_need("invalid tile Need flags");
    std::vector<TileGrid> grid;
    if (query.input_tile_grids.empty()) {
      auto declared = TileGrid::members(
          *query.inputs[input.input].result_schema, query.tile_extent);
      if (!declared.ok())
        return declared.status();
      grid = declared.take_value();
    } else {
      if (query.input_tile_grids.size() != query.inputs.size())
        return invalid_result_need("incomplete compiled input grids");
      grid = query.input_tile_grids[input.input];
    }
    auto checked = input.resolve(grid, resources);
    if (!checked.ok())
      return checked.status().code == ErrorCode::InvalidArgument
                 ? invalid_result_need("tile Need is outside the compiled grid")
                 : checked.status();
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
