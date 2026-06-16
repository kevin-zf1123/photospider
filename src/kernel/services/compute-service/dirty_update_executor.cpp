#include "kernel/services/compute-service/dirty_update_executor.hpp"

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "kernel/graph_runtime.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"
#include "kernel/services/compute-service/dirty_execution_common.hpp"
#include "kernel/services/compute-service/dirty_node_executor.hpp"
#include "kernel/services/compute-service/dirty_region_planner.hpp"
#include "kernel/services/compute-service/downsample_executor.hpp"
#include "kernel/services/graph_event_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"
#include "kernel/services/roi_propagation_service.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Clips an HP dirty entry to a planned task ROI.
 *
 * @param entry Base HP entry selected by dirty planning.
 * @param task Planned task whose ROI should bound execution.
 * @return Entry copy scoped to the task output ROI.
 * @throws Nothing directly.
 * @note Tile tasks execute one tile; monolithic/node tasks keep planner ROI.
 */
HpPlanEntry entry_for_task(const HpPlanEntry& entry, const PlannedTask& task) {
  HpPlanEntry clipped = entry;
  if (task.kind == PlannedTaskKind::Tile && task.output_roi.width > 0 &&
      task.output_roi.height > 0) {
    clipped.roi_hp = clip_rect(task.output_roi, entry.hp_size);
  }
  return clipped;
}

/**
 * @brief Clips an RT dirty entry to a planned task ROI.
 *
 * @param entry Base RT entry selected by dirty planning.
 * @param task Planned task whose domain-local ROI should bound execution.
 * @return Entry copy scoped to the task output ROI.
 * @throws Nothing directly.
 * @note RT task output_roi is already in RT execution coordinates; HP ROI is
 * conservatively preserved for commit/inspection metadata.
 */
RtPlanEntry entry_for_task(const RtPlanEntry& entry, const PlannedTask& task) {
  RtPlanEntry clipped = entry;
  if (task.kind == PlannedTaskKind::Tile && task.output_roi.width > 0 &&
      task.output_roi.height > 0) {
    clipped.roi_rt = clip_rect(task.output_roi, entry.rt_size);
  }
  return clipped;
}

/**
 * @brief Creates a scheduler task that executes a planned dirty task entry.
 *
 * @tparam EntryMap Unordered map from node id to HP or RT plan entry.
 * @tparam ExecuteNode Callable that receives node id, base entry, and
 * PlannedTask.
 * @param runtime Optional runtime used only for exception trace events.
 * @param plan Plan entry map selected by dirty planning.
 * @param compute_plan Dirty-pruned plan containing task metadata.
 * @param task_id Task id requested by source-first task dispatch.
 * @param execute_node Callable that runs the dirty executor for the task.
 * @return Scheduler callback for one dirty task.
 * @throws std::bad_alloc if the closure allocation fails.
 * @note Missing plan entries remain no-ops, matching previous wrapper
 * behavior for pruned or stale dirty work.
 */
template <typename EntryMap, typename ExecuteNode>
SchedulerTaskRuntime::Task make_planned_dirty_task(
    GraphRuntime* runtime, EntryMap& plan, const ComputePlan& compute_plan,
    int task_id, ExecuteNode execute_node) {
  return [runtime, &plan, &compute_plan, task_id, execute_node]() {
    if (task_id < 0 ||
        task_id >= static_cast<int>(compute_plan.task_graph.tasks.size())) {
      return;
    }
    const PlannedTask& task = compute_plan.task_graph.tasks[task_id];
    auto entry_it = plan.find(task.node_id);
    if (entry_it == plan.end()) {
      return;
    }
    try {
      execute_node(task.node_id, entry_it->second, task);
    } catch (...) {
      if (runtime) {
        runtime->log_event(GraphRuntime::SchedulerEvent::RETHROW_EXCEPTION,
                           task.node_id);
      }
      throw;
    }
  };
}

/**
 * @brief Builds request-local locks for graph nodes present in a dirty plan.
 *
 * @param compute_plan Dirty-pruned plan whose planned work will be executed.
 * @return Mutex map keyed by node id for shared cache allocation and commit.
 * @throws std::bad_alloc if mutex allocation fails.
 * @note Locks are intentionally request-local so sibling HP/RT dirty graphs do
 * not share scheduler task state or commit locks across intents.
 */
DirtyNodeMutexMap make_dirty_node_mutexes(const ComputePlan& compute_plan) {
  DirtyNodeMutexMap mutexes;
  mutexes.reserve(compute_plan.planned_work.size());
  for (const auto& work : compute_plan.planned_work) {
    mutexes.emplace(work.node_id, std::make_shared<std::mutex>());
  }
  return mutexes;
}

}  // namespace

HighPrecisionDirtyExecutor::HighPrecisionDirtyExecutor(
    GraphTraversalService& traversal, GraphEventService& events)
    : traversal_(traversal), events_(events) {}

void HighPrecisionDirtyExecutor::reset_plan_cache(
    GraphModel& graph, const HighPrecisionDirtyPlan& plan) const {
  for (const auto& [node_id, entry] : plan.entries) {
    (void)entry;
    Node& node = graph.mutable_node(node_id);
    node.cached_output_high_precision.reset();
    node.hp_roi.reset();
    node.hp_version = 0;
  }
}

NodeOutput& HighPrecisionDirtyExecutor::require_target_output(
    GraphModel& graph, int node_id) const {
  Node& target = graph.mutable_node(node_id);
  if (!target.cached_output_high_precision) {
    throw GraphError(GraphErrc::ComputeError,
                     "HP compute finished without target output.");
  }
  return *target.cached_output_high_precision;
}

NodeOutput& HighPrecisionDirtyExecutor::execute(
    GraphModel& graph, GraphRuntime* runtime,
    const DirtyUpdateRequest& request) {
  [[maybe_unused]] std::unique_lock<std::mutex> graph_lock(graph.graph_mutex_);

  RoiPropagationService roi_propagation;
  DirtyRegionPlanner dirty_planner(traversal_, roi_propagation);
  auto prepared = prepare_dirty_execution(
      graph,
      dirty_planner.plan_high_precision(graph, request.node_id,
                                        request.dirty_roi),
      ComputeRequest{ComputeIntent::GlobalHighPrecision, request.node_id, false,
                     request.dirty_roi});
  HighPrecisionDirtyPlan& dirty_plan = prepared.dirty_plan;
  if (request.force_recache) {
    reset_plan_cache(graph, dirty_plan);
  }

  std::vector<DownsampleExecutor::Request> downsample_requests;
  std::mutex downsample_requests_mutex;
  DirtyNodeMutexMap node_mutexes =
      make_dirty_node_mutexes(prepared.compute_plan);
  DirtyNodeExecutionContext node_context{graph, runtime, events_,
                                         dirty_plan.snapshot,
                                         dirty_plan.snapshot.graph_generation,
                                         node_mutexes};
  HighPrecisionDirtyNodeExecutor node_executor(
      node_context,
      DownsampleRequestSink{downsample_requests, downsample_requests_mutex});

  auto make_hp_task = [&](int task_id) -> SchedulerTaskRuntime::Task {
    return make_planned_dirty_task(
        runtime, dirty_plan.entries, prepared.compute_plan, task_id,
        [&](int node_id, HpPlanEntry& entry, const PlannedTask& task) {
          Node& node = graph.mutable_node(node_id);
          HpPlanEntry task_entry = entry_for_task(entry, task);
          node_executor.execute(node, task_entry);
        });
  };
  auto validate_hp_source_boundaries = [&]() {
    validate_dirty_source_boundaries_ready(graph, dirty_plan.snapshot,
                                           DirtyDomain::HighPrecision);
  };

  run_dirty_source_first(
      DirtySourceFirstRunRequest{
          runtime, ComputeIntent::GlobalHighPrecision, &prepared.compute_plan,
          &prepared.source_task_ids, &prepared.downstream_task_ids,
          dirty_plan.snapshot.graph_generation, validate_hp_source_boundaries},
      make_hp_task);
  DownsampleExecutor(graph, runtime, events_).execute(downsample_requests);
  return require_target_output(graph, request.node_id);
}

RealTimeDirtyExecutor::RealTimeDirtyExecutor(GraphTraversalService& traversal,
                                             GraphEventService& events)
    : traversal_(traversal), events_(events) {}

void RealTimeDirtyExecutor::reset_plan_cache(
    GraphModel& graph, const RealTimeDirtyPlan& plan) const {
  for (const auto& [node_id, entry] : plan.entries) {
    (void)entry;
    Node& node = graph.mutable_node(node_id);
    node.cached_output_real_time.reset();
    node.rt_roi.reset();
    node.rt_version = 0;
  }
}

NodeOutput& RealTimeDirtyExecutor::require_target_output(GraphModel& graph,
                                                         int node_id) const {
  Node& target = graph.mutable_node(node_id);
  if (!target.cached_output_real_time) {
    throw GraphError(GraphErrc::ComputeError,
                     "RT compute finished without target output.");
  }
  return *target.cached_output_real_time;
}

NodeOutput& RealTimeDirtyExecutor::execute(GraphModel& graph,
                                           GraphRuntime* runtime,
                                           const DirtyUpdateRequest& request) {
  [[maybe_unused]] std::unique_lock<std::mutex> graph_lock(graph.graph_mutex_);

  RoiPropagationService roi_propagation;
  DirtyRegionPlanner dirty_planner(traversal_, roi_propagation);
  auto prepared = prepare_dirty_execution(
      graph,
      dirty_planner.plan_real_time(graph, request.node_id, request.dirty_roi),
      ComputeRequest{ComputeIntent::RealTimeUpdate, request.node_id, false,
                     request.dirty_roi});
  RealTimeDirtyPlan& dirty_plan = prepared.dirty_plan;
  if (request.force_recache) {
    reset_plan_cache(graph, dirty_plan);
  }

  DirtyNodeMutexMap node_mutexes =
      make_dirty_node_mutexes(prepared.compute_plan);
  DirtyNodeExecutionContext node_context{graph, runtime, events_,
                                         dirty_plan.snapshot,
                                         dirty_plan.snapshot.graph_generation,
                                         node_mutexes};
  RealTimeDirtyNodeExecutor node_executor(node_context);
  auto make_rt_task = [&](int task_id) -> SchedulerTaskRuntime::Task {
    return make_planned_dirty_task(
        runtime, dirty_plan.entries, prepared.compute_plan, task_id,
        [&](int node_id, RtPlanEntry& entry, const PlannedTask& task) {
          Node& node = graph.mutable_node(node_id);
          RtPlanEntry task_entry = entry_for_task(entry, task);
          node_executor.execute(node, task_entry);
        });
  };
  auto validate_rt_source_boundaries = [&]() {
    validate_dirty_source_boundaries_ready(graph, dirty_plan.snapshot,
                                           DirtyDomain::RealTime);
  };

  run_dirty_source_first(
      DirtySourceFirstRunRequest{
          runtime, ComputeIntent::RealTimeUpdate, &prepared.compute_plan,
          &prepared.source_task_ids, &prepared.downstream_task_ids,
          dirty_plan.snapshot.graph_generation, validate_rt_source_boundaries},
      make_rt_task);
  return require_target_output(graph, request.node_id);
}

}  // namespace ps::compute
