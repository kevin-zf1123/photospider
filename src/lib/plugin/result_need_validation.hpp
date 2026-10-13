#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>

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
  const auto count = need.tiles.size();
  if ((count == 0 && !allow_empty) || count > maximum ||
      (!need.tiles.empty() && !need.tiles.get_allocator().owned_by(resources)))
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
    std::optional<TileGrid> grid;
    if (query.input_tile_grids.empty()) {
      auto declared = TileGrid::from_schema(
          *query.inputs[input.input].result_schema, query.tile_extent);
      if (!declared.ok())
        return declared.status();
      grid = declared.take_value();
    } else {
      if (query.input_tile_grids.size() != query.inputs.size())
        return invalid_result_need("incomplete compiled input grids");
      grid = query.input_tile_grids[input.input];
    }
    auto checked = input.resolve(grid, grid.has_value(), resources);
    if (!checked.ok())
      return checked.status().code == ErrorCode::InvalidArgument
                 ? invalid_result_need("tile Need is outside the compiled grid")
                 : checked.status();
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
