#pragma once

#include "photospider/execution/execution.hpp"

namespace ps::execution_internal {
// Compares the complete logical units named by observed workflow-input Needs.
// Input indices address this plan's declarations, never binding-vector order.
// Only semantic facts and logical bytes count as edits; progress and physical
// storage choices do not. The returned sparse changes own Root metadata.
Result<ResourceVector<TileChange>> tile_binding_changes(
    const ExecutionPlan& plan, const ExecutionBindings& before,
    const ExecutionBindings& after,
    const ResourceVector<TileDependency>& observed, const ResourceBudget& root,
    const SnapshotAccessOptions& options);
}  // namespace ps::execution_internal
