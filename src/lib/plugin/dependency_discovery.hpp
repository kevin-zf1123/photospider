#pragma once

#include <cstdint>
#include <vector>

#include "photospider/plugin/dependency_program.hpp"

namespace ps::plugin_internal {
/** @brief Decodes a completed immutable discovery table under exact bounds. */
Result<std::vector<DependencyNeed>> decode_discovery(
    const CpuStorage& table, std::uint32_t capacity, std::uint32_t candidates,
    const DependencyQuery& query, const FootprintLimits& limits,
    std::uint64_t* normalization_work, std::uint64_t* metadata_entries);
}  // namespace ps::plugin_internal
