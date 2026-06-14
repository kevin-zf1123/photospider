#include "kernel/services/compute-service/dirty_update_executor.hpp"

#include <mutex>
#include <utility>
#include <vector>

#include "kernel/graph_runtime.hpp"
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
 * @brief Creates a scheduler task that executes a planned node entry.
 *
 * @tparam EntryMap Unordered map from node id to HP or RT plan entry.
 * @tparam ExecuteNode Callable that receives node id and the matching plan
 * entry.
 * @param runtime Optional runtime used only for exception trace events.
 * @param plan Plan entry map selected by dirty planning.
 * @param task_node_id Node id requested by source-first task dispatch.
 * @param execute_node Callable that runs the node-level dirty executor.
 * @return Scheduler callback for one dirty node.
 * @throws std::bad_alloc if the closure allocation fails.
 * @note Missing plan entries remain no-ops, matching the previous dirty task
 * wrapper behavior after task-id-to-node collapse.
 */
template <typename EntryMap, typename ExecuteNode>
SchedulerTaskRuntime::Task make_planned_node_task(GraphRuntime* runtime,
                                                  EntryMap& plan,
                                                  int task_node_id,
                                                  ExecuteNode execute_node) {
  return [runtime, &plan, task_node_id, execute_node]() {
    auto entry_it = plan.find(task_node_id);
    if (entry_it == plan.end()) {
      return;
    }
    try {
      execute_node(task_node_id, entry_it->second);
    } catch (...) {
      if (runtime) {
        runtime->log_event(GraphRuntime::SchedulerEvent::RETHROW_EXCEPTION,
                           task_node_id);
      }
      throw;
    }
  };
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
  DirtyNodeExecutionContext node_context{graph, runtime, events_,
                                         dirty_plan.snapshot,
                                         dirty_plan.snapshot.graph_generation};
  HighPrecisionDirtyNodeExecutor node_executor(
      node_context,
      DownsampleRequestSink{downsample_requests, downsample_requests_mutex});

  auto make_hp_task = [&](int task_node_id) -> SchedulerTaskRuntime::Task {
    return make_planned_node_task(runtime, dirty_plan.entries, task_node_id,
                                  [&](int node_id, HpPlanEntry& entry) {
                                    Node& node = graph.mutable_node(node_id);
                                    node_executor.execute(node, entry);
                                  });
  };
  auto validate_hp_source_boundaries = [&]() {
    validate_dirty_source_boundaries_ready(graph, dirty_plan.snapshot,
                                           DirtyDomain::HighPrecision);
  };

  run_dirty_source_first(
      DirtySourceFirstRunRequest{
          runtime, ComputeIntent::GlobalHighPrecision,
          &prepared.source_node_ids, &prepared.downstream_node_ids,
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

  DirtyNodeExecutionContext node_context{graph, runtime, events_,
                                         dirty_plan.snapshot,
                                         dirty_plan.snapshot.graph_generation};
  RealTimeDirtyNodeExecutor node_executor(node_context);
  auto make_rt_task = [&](int task_node_id) -> SchedulerTaskRuntime::Task {
    return make_planned_node_task(runtime, dirty_plan.entries, task_node_id,
                                  [&](int node_id, RtPlanEntry& entry) {
                                    Node& node = graph.mutable_node(node_id);
                                    node_executor.execute(node, entry);
                                  });
  };
  auto validate_rt_source_boundaries = [&]() {
    validate_dirty_source_boundaries_ready(graph, dirty_plan.snapshot,
                                           DirtyDomain::RealTime);
  };

  run_dirty_source_first(
      DirtySourceFirstRunRequest{
          runtime, ComputeIntent::RealTimeUpdate, &prepared.source_node_ids,
          &prepared.downstream_node_ids, dirty_plan.snapshot.graph_generation,
          validate_rt_source_boundaries},
      make_rt_task);
  return require_target_output(graph, request.node_id);
}

}  // namespace ps::compute
