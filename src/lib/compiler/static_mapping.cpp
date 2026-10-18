#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"

namespace ps {
Result<ResultStaticMapping> complete_input_dependencies(
    const ResultStaticQuery& query) {
  ResultStaticMapping mapping;
  const auto& traits = query.prepared.traits();
  std::optional<std::vector<std::uint32_t>> projection;
  if (query.shared_stage == UINT32_MAX) {
    if (query.output_index >= traits.outputs.size())
      return Result<ResultStaticMapping>(
          {ErrorCode::InvalidArgument, "invalid static output"});
    projection = traits.outputs[query.output_index].input_indices;
  } else {
    if (query.shared_stage >= traits.shared_stages.size())
      return Result<ResultStaticMapping>(
          {ErrorCode::InvalidArgument, "invalid static stage"});
    projection = traits.shared_stages[query.shared_stage].input_indices;
  }
  for (std::uint32_t input = 0; input < query.metadata.inputs.size(); ++input) {
    if (projection && std::find(projection->begin(), projection->end(),
                                input) == projection->end())
      continue;
    const auto& grids = query.metadata.input_tile_grids[input];
    for (std::uint32_t slot = 0; slot < grids.size(); ++slot) {
      auto all = TileSet::range(ResourceBudget{}, 0, grids[slot].tile_count());
      if (!all.ok())
        return Result<ResultStaticMapping>(all.status());
      mapping.inputs.push_back({input, slot, false, all.take_value()});
    }
  }
  return Result<ResultStaticMapping>(std::move(mapping));
}
}  // namespace ps
