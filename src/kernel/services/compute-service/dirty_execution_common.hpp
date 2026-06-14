#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph_model.hpp"  // NOLINT(build/include_subdir)
#include "kernel/scheduler/scheduler_task_runtime.hpp"
#include "kernel/services/compute-service/compute_task_dispatcher.hpp"
#include "kernel/services/compute-service/dirty_region_planner.hpp"
#include "kernel/services/compute-service/task_graph_planning.hpp"
#include "ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps {
class GraphModel;
class GraphRuntime;
}  // namespace ps

namespace ps::compute {

/**
 * @brief Returns a running scheduler task runtime for one compute intent.
 *
 * This helper preserves the ComputeService scheduler contract used by full
 * graph dispatch and dirty ROI dispatch. It looks up the scheduler registered
 * on the supplied GraphRuntime, verifies that it implements
 * SchedulerTaskRuntime, starts it when necessary, and returns the task-runtime
 * facade that accepts concrete ready task callbacks.
 *
 * @param runtime Per-graph runtime that owns intent-specific schedulers.
 * @param intent Compute intent whose scheduler should receive work.
 * @return Running SchedulerTaskRuntime for the requested intent.
 * @throws GraphError when no scheduler is registered or the registered
 * scheduler cannot dispatch planned task callbacks.
 * @note The returned reference remains owned by GraphRuntime. Callers must not
 * store it beyond the active compute request.
 */
SchedulerTaskRuntime& ensure_scheduler_task_runtime(GraphRuntime& runtime,
                                                    ComputeIntent intent);

/**
 * @brief Bounded dirty planning result used by HP and RT executors.
 *
 * The prepared state packages the graph-scoped dirty snapshot, the
 * node/cache-pruned and dirty-pruned compute plan, and the materialized
 * source/downstream node groups that will be submitted to the scheduler. The
 * dirty plan itself owns the per-node HP or RT ROI entries used by node
 * execution.
 *
 * @tparam DirtyPlan HighPrecisionDirtyPlan or RealTimeDirtyPlan.
 * @note The struct is request-local. It must not be stored after scheduler
 * callbacks derived from it have drained.
 */
template <typename DirtyPlan>
struct PreparedDirtyPlan {
  /** @brief Dirty planner output with per-node execution entries. */
  DirtyPlan dirty_plan;

  /** @brief Dirty-pruned compute plan recorded for inspection. */
  ComputePlan compute_plan;

  /** @brief Task id groups selected by DirtySnapshotTaskGraphPruner. */
  DirtyUpdateWorkSet work_set;

  /** @brief Dirty source node ids selected from materialized task ids. */
  std::vector<int> source_node_ids;

  /** @brief Downstream dirty node ids selected from materialized task ids. */
  std::vector<int> downstream_node_ids;
};

/**
 * @brief Immutable parameters for source-first dirty task dispatch.
 *
 * The request object lowers helper parameter count and makes the dispatch
 * contract explicit: source boundary work completes before downstream work,
 * and scheduler submissions carry the dirty generation as an epoch.
 */
struct DirtySourceFirstRunRequest {
  /** @brief Optional runtime that owns intent schedulers and trace events. */
  GraphRuntime* runtime = nullptr;

  /** @brief Intent-specific scheduler task pool to target. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;

  /** @brief Dirty source node ids submitted before downstream work. */
  const std::vector<int>* source_node_ids = nullptr;

  /** @brief Downstream dirty node ids submitted after source validation. */
  const std::vector<int>* downstream_node_ids = nullptr;

  /** @brief Dirty snapshot generation forwarded as scheduler epoch metadata. */
  uint64_t dirty_generation = 0;

  /** @brief Boundary validation invoked between source and downstream groups.
   */
  std::function<void()> before_downstream;
};

/**
 * @brief Stores the latest dirty snapshot and bounded history on the graph.
 *
 * @param graph Graph whose inspection state receives the snapshot.
 * @param snapshot Dirty-region snapshot generated for the current request.
 * @throws std::bad_alloc if snapshot history storage cannot grow.
 * @note The history cap mirrors ComputeService's existing inspection policy.
 */
void remember_dirty_snapshot(GraphModel& graph,
                             const DirtyRegionSnapshot& snapshot);

/**
 * @brief Stores the latest compute plan and bounded recent-plan history.
 *
 * @param graph Graph whose inspection state receives the compute plan.
 * @param compute_plan Dirty-pruned plan for the current request.
 * @throws std::bad_alloc if plan history storage cannot grow.
 * @note Plans are diagnostic state, not an unbounded runtime trace.
 */
void remember_compute_plan(GraphModel& graph, const ComputePlan& compute_plan);

/**
 * @brief Prunes a full task graph to the current request and cache state.
 *
 * @param graph Graph that supplies topology, node metadata, and cache state.
 * @param request Intent, target node, and dirty ROI for the request.
 * @param execution_order Topological order selected by dirty planning.
 * @return Node/cache-pruned compute plan before dirty snapshot selection.
 * @throws GraphError from task graph expansion or pruning.
 * @note The returned plan is still domain-specific and contains no mixed HP/RT
 * task pool.
 */
ComputePlan prune_node_cache_task_graph(
    const GraphModel& graph, const ComputeRequest& request,
    const std::vector<int>& execution_order);

/**
 * @brief Applies dirty snapshot pruning to a node/cache-pruned plan.
 *
 * @param node_cache_plan Plan already scoped to target and cache state.
 * @param snapshot Dirty snapshot for the same compute domain.
 * @return Dirty-pruned plan with selected tasks annotated.
 * @throws GraphError from dirty snapshot pruning.
 * @note This helper does not create new tasks; it only selects or clips tasks
 * already expanded in the request plan.
 */
ComputePlan prune_dirty_snapshot_task_graph(
    const ComputePlan& node_cache_plan, const DirtyRegionSnapshot& snapshot);

/**
 * @brief Resolves task ids from a dirty work set back to unique node ids.
 *
 * @param compute_plan Plan containing task-to-node ownership.
 * @param task_ids Dirty source or downstream task ids selected by materialize.
 * @return Node ids in planned-work order with duplicates removed.
 * @throws std::bad_alloc if temporary set or vector allocation fails.
 * @note Dirty execution still runs at node granularity today, so selected tile
 * task ids are collapsed to node ids before callbacks are built.
 */
std::vector<int> planned_nodes_for_task_ids(const ComputePlan& compute_plan,
                                            const std::vector<int>& task_ids);

/**
 * @brief Verifies dirty source boundary outputs before downstream work starts.
 *
 * @param graph Graph whose dirty source nodes are inspected.
 * @param snapshot Dirty snapshot containing source node ids.
 * @param domain HP or RT dirty domain used to select cache authority.
 * @throws GraphError when a dirty source node is missing or its boundary
 * output is unavailable.
 * @note The check runs after source tasks and before downstream tasks to keep
 * dependency failures deterministic across inline and scheduler execution.
 */
void validate_dirty_source_boundaries_ready(const GraphModel& graph,
                                            const DirtyRegionSnapshot& snapshot,
                                            DirtyDomain domain);

/**
 * @brief Checks whether a node is listed as a dirty source in a snapshot.
 *
 * @param snapshot Dirty snapshot for the current request.
 * @param node_id Node id being inspected.
 * @return True when the node is a dirty source boundary.
 * @throws Nothing directly.
 * @note Source membership controls stale-generation checks and trace labels.
 */
bool is_dirty_source_node(const DirtyRegionSnapshot& snapshot, int node_id);

/**
 * @brief Logs generic and dirty-role execution events for one node.
 *
 * @param runtime Optional runtime that owns the scheduler trace log.
 * @param node_id Node being executed.
 * @param dirty_source Whether the node is a source boundary or downstream
 * dirty work.
 * @throws Any exception propagated by GraphRuntime::log_event.
 * @note A null runtime preserves inline execution behavior by emitting no
 * scheduler trace entries.
 */
void log_dirty_node_execution(GraphRuntime* runtime, int node_id,
                              bool dirty_source);

/**
 * @brief Logs and skips stale dirty source generations.
 *
 * @param runtime Optional runtime for scheduler trace events.
 * @param node_id Dirty source node id.
 * @param committed_generation Generation already committed for this source.
 * @param dirty_generation Generation being executed by the current request.
 * @return True when the current request should skip the node.
 * @throws Any exception propagated by GraphRuntime::log_event.
 * @note The comparison intentionally preserves the strict-greater policy so
 * repeated execution of the same generation is still allowed.
 */
bool should_skip_stale_dirty_source(GraphRuntime* runtime, int node_id,
                                    uint64_t committed_generation,
                                    uint64_t dirty_generation);

/**
 * @brief Infers image channels and data type for a reused or new output buffer.
 *
 * @param preferred Existing output preferred for the target intent.
 * @param image_inputs Ready image inputs for the node.
 * @param fallback Optional secondary output used as a final shape hint.
 * @return Pair of channel count and data type.
 * @throws Nothing directly.
 * @note Defaults to one FLOAT32 channel when neither output nor input carries
 * concrete image metadata, matching the pre-split dirty update behavior.
 */
std::pair<int, DataType> infer_output_spec(
    const std::optional<NodeOutput>& preferred,
    const std::vector<const NodeOutput*>& image_inputs,
    const std::optional<NodeOutput>* fallback = nullptr);

/**
 * @brief Applies dirty-pruned HP ROI overrides back to HP plan entries.
 *
 * @param entries Per-node HP execution entries from DirtyRegionPlanner.
 * @param compute_plan Dirty-pruned compute plan selected for execution.
 * @throws Nothing directly.
 * @note DirtySnapshotTaskGraphPruner may further clip represented HP ROIs; the
 * executor must use those clipped regions for node execution.
 */
void apply_planned_work_rois(std::unordered_map<int, HpPlanEntry>& entries,
                             const ComputePlan& compute_plan);

/**
 * @brief Applies dirty-pruned HP and RT ROI overrides back to RT plan entries.
 *
 * @param entries Per-node RT execution entries from DirtyRegionPlanner.
 * @param compute_plan Dirty-pruned compute plan selected for execution.
 * @throws Nothing directly.
 * @note HP-space ROI is used for inspection/version metadata, while execution
 * ROI is used to clip RT proxy buffer writes.
 */
void apply_planned_work_rois(std::unordered_map<int, RtPlanEntry>& entries,
                             const ComputePlan& compute_plan);

/**
 * @brief Prepares common dirty execution state after planner output exists.
 *
 * @tparam DirtyPlan HighPrecisionDirtyPlan or RealTimeDirtyPlan.
 * @param graph Graph whose inspection state receives the plan and snapshot.
 * @param dirty_plan Dirty planner output for one intent domain.
 * @param request Compute request matching the same dirty domain.
 * @return Prepared plan with node groups ready for task construction.
 * @throws GraphError from task graph pruning or materialization.
 * @note The helper updates graph inspection fields before execution so failed
 * execution still leaves the latest planning evidence visible to callers.
 */
template <typename DirtyPlan>
PreparedDirtyPlan<DirtyPlan> prepare_dirty_execution(
    GraphModel& graph, DirtyPlan&& dirty_plan, const ComputeRequest& request) {
  graph.last_dirty_region_snapshot_debug =
      DirtyRegionPlanner::describe_snapshot(dirty_plan.snapshot);
  remember_dirty_snapshot(graph, dirty_plan.snapshot);

  const ComputePlan node_cache_plan =
      prune_node_cache_task_graph(graph, request, dirty_plan.execution_order);
  ComputePlan compute_plan =
      prune_dirty_snapshot_task_graph(node_cache_plan, dirty_plan.snapshot);
  apply_planned_work_rois(dirty_plan.entries, compute_plan);
  remember_compute_plan(graph, compute_plan);

  DirtySnapshotTaskGraphPruner dirty_snapshot_pruner;
  DirtyUpdateWorkSet work_set =
      dirty_snapshot_pruner.materialize(compute_plan, dirty_plan.snapshot);
  std::vector<int> source_node_ids =
      planned_nodes_for_task_ids(compute_plan, work_set.dirty_source_task_ids);
  std::vector<int> downstream_node_ids =
      planned_nodes_for_task_ids(compute_plan, work_set.downstream_task_ids);

  return PreparedDirtyPlan<DirtyPlan>{
      std::move(dirty_plan), std::move(compute_plan), std::move(work_set),
      std::move(source_node_ids), std::move(downstream_node_ids)};
}

/**
 * @brief Runs dirty source tasks before downstream dirty tasks.
 *
 * @tparam MakeTask Callable that turns a node id into
 * SchedulerTaskRuntime::Task.
 * @param request Source-first dispatch request and boundary validation.
 * @param make_task Factory for node execution closures.
 * @throws Exceptions from task construction, task execution, boundary
 * validation, scheduler lookup, or scheduler submission.
 * @note Ordering intentionally mirrors the pre-split ComputeService logic:
 * all source tasks finish before downstream tasks are released.
 */
template <typename MakeTask>
void run_dirty_source_first(const DirtySourceFirstRunRequest& request,
                            MakeTask make_task) {
  const std::vector<int>& source_node_ids = *request.source_node_ids;
  const std::vector<int>& downstream_node_ids = *request.downstream_node_ids;
  if (request.runtime) {
    SchedulerTaskRuntime& dirty_task_runtime =
        ensure_scheduler_task_runtime(*request.runtime, request.intent);
    std::vector<SchedulerTaskRuntime::Task> source_tasks;
    std::vector<SchedulerTaskRuntime::Task> downstream_tasks;
    source_tasks.reserve(source_node_ids.size());
    downstream_tasks.reserve(downstream_node_ids.size());
    for (int source_node_id : source_node_ids) {
      source_tasks.push_back(make_task(source_node_id));
    }
    for (int downstream_node_id : downstream_node_ids) {
      downstream_tasks.push_back(make_task(downstream_node_id));
    }
    ComputeTaskDispatcher::submit_dirty_ready_tasks_source_first(
        dirty_task_runtime, std::move(source_tasks),
        std::move(downstream_tasks), request.dirty_generation,
        request.before_downstream);
    return;
  }

  for (int source_node_id : source_node_ids) {
    make_task(source_node_id)();
  }
  if (request.before_downstream) {
    request.before_downstream();
  }
  for (int downstream_node_id : downstream_node_ids) {
    make_task(downstream_node_id)();
  }
}

}  // namespace ps::compute
