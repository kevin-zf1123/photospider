#pragma once

#include "kernel/services/compute-service/task_graph_planning.hpp"

namespace ps {
class GraphModel;
}

namespace ps::compute {

/**
 * @brief Applies dirty snapshot metadata to a single planned task.
 *
 * @param task Task whose dirty flags, source-boundary flag, and generation are
 * updated.
 * @param snapshot Optional graph-scoped dirty snapshot for the current plan.
 * @throws Nothing directly.
 * @note A null snapshot marks tasks as selected compatibility work with
 * generation zero.
 */
void apply_task_dirty_metadata(PlannedTask& task,
                               const DirtyRegionSnapshot* snapshot);

/**
 * @brief Selects task population strategy for graph-backed and graphless plans.
 *
 * TaskPopulationStrategy owns the branch previously embedded in
 * populate_tasks(): graph-backed full expansion and graphless node-only
 * fallback. It also routes HP/RT implementation checks through a domain shape
 * strategy so task creation remains single-domain.
 *
 * @note The strategy appends tasks to the supplied ComputePlan and updates
 * PlannedNodeWork::task_ids through the same task ids. It does not build task
 * dependencies; callers must run populate_task_dependencies() afterwards.
 */
class TaskPopulationStrategy {
 public:
  /**
   * @brief Populates executable task records for the current plan.
   *
   * @param result Plan receiving appended PlannedTask entries.
   * @param snapshot Optional dirty snapshot used only for dirty metadata.
   * @param domain HP or RT compute domain for this single-domain plan.
   * @param graph Optional graph used to resolve output extents and op shapes.
   * @throws GraphError or standard exceptions from graph lookups, extent
   * resolution, operation metadata lookup, or vector growth.
   * @note Dirty snapshots never create task shapes. Graph-backed plans derive
   * shapes from graph/op metadata; graphless plans use one fallback node task
   * per planned work item.
   */
  void populate(ComputePlan& result, const DirtyRegionSnapshot* snapshot,
                DirtyDomain domain, const GraphModel* graph) const;
};

}  // namespace ps::compute
