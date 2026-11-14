#pragma once

#include <map>
#include <string>
#include <vector>

#include "photospider/compiler/planner.hpp"

namespace ps::compiler_internal {
struct StaticPlanningInput {
  physical::Program& program;
  const std::vector<PlanCallBinding>& calls;
  const std::map<std::string, std::size_t>& outputs;
  const std::map<std::string, TileRequest>& output_requests;
  std::function<bool(physical::Id, bool)> trait;
  std::function<physical::Domain(physical::Id)> input_domain;
  std::function<Result<OperationProgram>(
      physical::Id, const std::vector<TileRequest>&, physical::Backend)>
      lower;
  std::function<bool(uint64_t)> charge;
};
Status expand_static(StaticPlanningInput& input,
                     const PlanningOptions& options);
PlannerDefinition static_tile_planner();
}  // namespace ps::compiler_internal
