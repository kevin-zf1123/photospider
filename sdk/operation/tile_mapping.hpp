#pragma once
#include "photospider/plugin/operation_registry.hpp"
namespace ps::plugin_internal {
Result<OperationProgram> lower_tilewise(const OperationLowering& query,
                                        const TilewiseLowering& definition,
                                        const OperationTraits& traits);
Result<std::vector<TileNeed>> solve_mapping(
    const MappingQuery& query, const MappingSolver& solver,
    const LoweringSession& session, const physical::ValidationLimits& limits);
}  // namespace ps::plugin_internal
