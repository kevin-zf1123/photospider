#pragma once
#include <cstdint>
#include <string>

#include "photospider/execution/execution.hpp"
namespace ps::execution_internal {
struct DemandKey {
  std::string value;
  std::uint64_t entries;
  ResourceLease lease;
};
Result<DemandKey> demand_key(const DemandQuery&, const ExecutionPlan&,
                             std::uint64_t, const ResourceBudget&);
// Decode explicit producer units into data-window coverage. This does not
// infer tile indices from sample coordinates.
Result<Footprint> demand_samples(const TileRequest&, const PlanStep&,
                                 std::uint32_t, const FootprintLimits&);
}  // namespace ps::execution_internal
