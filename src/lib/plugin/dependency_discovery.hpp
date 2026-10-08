#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "photospider/plugin/result_program.hpp"

namespace ps::plugin_internal {
struct GpuDiscoveryRecord final {
  std::uint32_t input = 0, roles = 0, rank = 0, slot = 0;
  std::array<std::uint64_t, 8> offsets{}, extents{};
};
Status visit_discovery_records(
    const CpuStorage& table, std::uint32_t capacity, std::uint32_t candidates,
    const FootprintLimits& limits,
    const std::function<Status(const GpuDiscoveryRecord&)>& visit);
Result<ResourceVector<ResultTensorNeed>> decode_result_discovery(
    const CpuStorage& table, std::uint32_t capacity, std::uint32_t candidates,
    const ResultProgramQuery& query, const ResourceBudget& resources,
    FootprintLimits limits, std::uint64_t* metadata_entries);
}  // namespace ps::plugin_internal
