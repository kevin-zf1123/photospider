#pragma once
#include <cstdint>
#include <string>

#include "photospider/execution/execution.hpp"
namespace ps::execution_internal {
struct DemandKey {
  std::string value;
  std::uint64_t entries;
};
Result<DemandKey> demand_key(const DemandQuery&, const ExecutionPlan&,
                             std::uint64_t);
}  // namespace ps::execution_internal
