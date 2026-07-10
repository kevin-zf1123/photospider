#include "kernel/services/compute-service/dirty_update_executor.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kernel/graph_runtime.hpp"
#include "kernel/services/compute-service/compute_cache_policy.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"
#include "kernel/services/compute-service/dirty_execution_common.hpp"
#include "kernel/services/compute-service/dirty_node_executor.hpp"
#include "kernel/services/compute-service/dirty_region_planner.hpp"
#include "kernel/services/compute-service/dirty_sibling_commit_gate.hpp"
#include "kernel/services/compute-service/dirty_write_buffers.hpp"
#include "kernel/services/compute-service/downsample_executor.hpp"
#include "kernel/services/compute-service/realtime_proxy_graph.hpp"
#include "kernel/services/graph_event_service.hpp"
#include "kernel/services/graph_extent_resolver.hpp"
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
 * @throws Nothing; HpPlanEntry contains only scalar/POD ROI metadata.
 * @note Tile tasks execute one tile; monolithic/node tasks keep planner ROI.
 * Copying and clipping this value performs no dynamic allocation.
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
 * @throws Nothing; RtPlanEntry contains only scalar/POD ROI metadata.
 * @note RT task output_roi is already in RT execution coordinates; HP ROI is
 * kept in the base entry for commit and inspection metadata. Copying and
 * clipping this value performs no dynamic allocation.
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
 * @brief Executes one planned dirty task entry by dense task id.
 *
 * @tparam EntryMap Unordered map from node id to HP or RT plan entry.
 * @tparam ExecuteNode Callable that receives node id, base entry, and
 * PlannedTask.
 * @param runtime Optional runtime used only for exception trace events.
 * @param plan Plan entry map selected by dirty planning.
 * @param compute_plan Dirty-pruned plan containing task metadata.
 * @param task_id Task id requested by source-first task dispatch.
 * @param execute_node Callable that runs the dirty executor for the task.
 * @return Nothing.
 * @throws std::bad_alloc unchanged from task lookup diagnostics or
 * execute_node.
 * @throws Exceptions propagated by execute_node.
 * @note Missing plan entries remain no-ops for pruned or stale dirty work.
 */
template <typename EntryMap, typename ExecuteNode>
void run_planned_dirty_task(GraphRuntime* runtime, EntryMap& plan,
                            const ComputePlan& compute_plan, int task_id,
                            ExecuteNode execute_node) {
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
}

/**
 * @brief Builds request-local locks for graph nodes present in a dirty plan.
 *
 * @param compute_plan Node/cache-pruned plan whose planned work will execute.
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

/**
 * @brief Validates HP dirty source boundaries against staged and graph output.
 *
 * @param graph Graph used for node lookup and committed fallback state.
 * @param snapshot Dirty snapshot containing source node ids.
 * @param hp_write_buffer Request-local HP output buffer populated by source
 * tasks before downstream HP work is released.
 * @return Nothing.
 * @throws GraphError when a source node is missing or has no staged/committed
 * HP output.
 * @throws std::bad_alloc unchanged if diagnostic construction exhausts memory.
 * @note HP dirty source output may still be staged, so validation cannot read
 * only GraphModel HP cache.
 */
void validate_hp_source_boundaries_ready(
    const GraphModel& graph, const DirtyRegionSnapshot& snapshot,
    const HighPrecisionDirtyWriteBuffer& hp_write_buffer) {
  for (int source_node_id : snapshot.dirty_source_nodes) {
    const Node* source = graph.find_node(source_node_id);
    if (!source) {
      throw GraphError(GraphErrc::NotFound, "Dirty source node " +
                                                std::to_string(source_node_id) +
                                                " not found.");
    }
    if (hp_write_buffer.has_output(source_node_id) ||
        ComputeCachePolicy::reusable_output(*source)) {
      continue;
    }
    throw GraphError(GraphErrc::MissingDependency,
                     "Dirty source boundary output is not ready for node " +
                         std::to_string(source_node_id) + ".");
  }
}

/**
 * @brief Validates RT dirty source boundaries against staged/proxy/HP output.
 *
 * @param graph Graph used for node lookup and committed fallback state.
 * @param proxy_graph Committed RT proxy graph used before HP fallback.
 * @param snapshot Dirty snapshot containing source node ids.
 * @param rt_write_buffer Request-local RT output buffer populated by source
 * tasks before downstream RT work is released.
 * @return Nothing.
 * @throws GraphError when a source node is missing or has no staged/committed
 * RT proxy or HP fallback output.
 * @throws std::bad_alloc unchanged if diagnostic construction exhausts memory.
 * @note RT dirty source output may still be staged, so validation checks the
 * request buffer before the committed proxy graph.
 */
void validate_rt_source_boundaries_ready(
    const GraphModel& graph, const RealtimeProxyGraph& proxy_graph,
    const DirtyRegionSnapshot& snapshot,
    const RealtimeProxyWriteBuffer& rt_write_buffer) {
  for (int source_node_id : snapshot.dirty_source_nodes) {
    const Node* source = graph.find_node(source_node_id);
    if (!source) {
      throw GraphError(GraphErrc::NotFound, "Dirty source node " +
                                                std::to_string(source_node_id) +
                                                " not found.");
    }
    if (rt_write_buffer.has_output(source_node_id) ||
        proxy_graph.find_output(source_node_id) ||
        ComputeCachePolicy::reusable_output(*source)) {
      continue;
    }
    throw GraphError(GraphErrc::MissingDependency,
                     "Dirty source boundary output is not ready for node " +
                         std::to_string(source_node_id) + ".");
  }
}

/**
 * @brief Selects the HP-space planning ROI for one HP dirty executor request.
 *
 * @param graph Graph containing the target HP cache used for forced full-frame
 * dirty planning.
 * @param request Dirty update request inherited from ComputeService.
 * @return Requested dirty ROI for normal updates, or the full target HP extent
 * for forced HP dirty updates.
 * @throws GraphError when a forced dirty update cannot derive a valid current
 * HP extent from the target node.
 * @throws std::bad_alloc unchanged when extent or diagnostic storage exhausts
 * memory.
 * @note Forced HP dirty updates do not seed existing HP output into the staging
 * buffer, so their dirty plan must cover the entire authoritative HP frame
 * before commit.
 */
cv::Rect hp_planning_roi_for_request(const GraphModel& graph,
                                     const DirtyUpdateRequest& request) {
  if (!request.force_recache) {
    return request.dirty_roi;
  }

  const Node* target = graph.find_node(request.node_id);
  if (!target) {
    throw GraphError(GraphErrc::NotFound,
                     "Cannot compute forced HP dirty update: node " +
                         std::to_string(request.node_id) + " not found.");
  }
  if (const NodeOutput* target_output =
          ComputeCachePolicy::reusable_output(*target)) {
    const ImageBuffer& buffer = target_output->image_buffer;
    if (buffer.width > 0 && buffer.height > 0) {
      return cv::Rect(0, 0, buffer.width, buffer.height);
    }
  }

  GraphExtentResolver extent_resolver;
  std::unordered_map<int, cv::Size> extent_cache;
  const cv::Size target_extent = extent_resolver.resolve_output_extent(
      graph, request.node_id, extent_cache);
  if (target_extent.width <= 0 || target_extent.height <= 0) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Cannot compute forced HP dirty update for node " +
                         std::to_string(request.node_id) +
                         ": HP output extent is unavailable.");
  }
  return cv::Rect(0, 0, target_extent.width, target_extent.height);
}

}  // namespace

/**
 * @brief Constructs the HP dirty executor from borrowed support services.
 *
 * @param traversal Traversal service used by dirty planning.
 * @param events Event sink used by node execution and downsample refresh.
 * @throws Nothing directly.
 * @note Both services must outlive this request-scoped executor.
 */
HighPrecisionDirtyExecutor::HighPrecisionDirtyExecutor(
    GraphTraversalService& traversal, GraphEventService& events)
    : traversal_(traversal), events_(events) {}

/**
 * @brief Clears HP cache state selected by one dirty plan.
 *
 * @param graph Graph whose planned nodes are reset.
 * @param plan HP dirty plan containing nodes to reset.
 * @return Nothing.
 * @throws GraphError if a planned node no longer exists.
 * @throws std::bad_alloc unchanged if graph lookup diagnostics allocate.
 * @note The caller owns graph-state serialization for the complete reset.
 */
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

/**
 * @brief Returns the committed HP target output after dirty execution.
 *
 * @param graph Graph owning the target HP cache.
 * @param node_id Target node selected by the internal service request.
 * @return Mutable committed HP output.
 * @throws GraphError when execution did not commit target output.
 * @throws std::bad_alloc unchanged if failure diagnostics allocate.
 * @note The returned reference remains graph-owned.
 */
NodeOutput& HighPrecisionDirtyExecutor::require_target_output(
    GraphModel& graph, int node_id) const {
  Node& target = graph.mutable_node(node_id);
  if (!target.cached_output_high_precision) {
    throw GraphError(GraphErrc::ComputeError,
                     "HP compute finished without target output.");
  }
  return *target.cached_output_high_precision;
}

/**
 * @brief Plans, executes, and commits one HP dirty request.
 *
 * @param graph Graph whose HP dirty state and cache are updated.
 * @param proxy_graph RT proxy graph receiving optional downsample refresh.
 * @param runtime Optional scheduler/trace owner; null executes work inline.
 * @param request Dirty target, ROI, cache, telemetry, and sibling-gate options.
 * @return Mutable target HP output owned by graph.
 * @throws std::bad_alloc unchanged when planning, task, cache, staging,
 * telemetry, or output storage exhausts memory.
 * @throws GraphError for planning, dependency, operation, scheduler, commit, or
 * target validation failures.
 * @note Planning and commit hold graph_mutex_ while scheduler work runs outside
 * that lock. All staging buffers and node mutexes are request-local.
 */
NodeOutput& HighPrecisionDirtyExecutor::execute(
    GraphModel& graph, RealtimeProxyGraph& proxy_graph, GraphRuntime* runtime,
    const DirtyUpdateRequest& request) {
  std::unique_lock<std::mutex> graph_lock(graph.graph_mutex_);

  if (request.force_recache) {
    graph.clear_full_task_graph_cache();
  }
  proxy_graph.synchronize_with_graph(graph);

  RoiPropagationService roi_propagation;
  DirtyRegionPlanner dirty_planner(traversal_, roi_propagation);
  const cv::Rect planning_roi = hp_planning_roi_for_request(graph, request);
  auto prepared = prepare_dirty_execution(
      graph,
      dirty_planner.plan_high_precision(graph, request.node_id, planning_roi),
      ComputeRequest{ComputeIntent::GlobalHighPrecision, request.node_id, false,
                     planning_roi});
  HighPrecisionDirtyPlan& dirty_plan = prepared.dirty_plan;
  graph_lock.unlock();

  HighPrecisionDirtyWriteBuffer hp_write_buffer(!request.force_recache);
  DirtyNodeMutexMap node_mutexes =
      make_dirty_node_mutexes(prepared.compute_plan);
  DirtyNodeExecutionContext node_context{graph,
                                         runtime,
                                         events_,
                                         dirty_plan.snapshot,
                                         dirty_plan.snapshot.graph_generation,
                                         node_mutexes};
  HighPrecisionDirtyNodeExecutor node_executor(node_context, hp_write_buffer);

  auto run_hp_task = [&](int task_id) {
    run_planned_dirty_task(
        runtime, dirty_plan.entries, prepared.compute_plan, task_id,
        [&](int node_id, HpPlanEntry& entry, const PlannedTask& task) {
          Node& node = graph.mutable_node(node_id);
          HpPlanEntry task_entry = entry_for_task(entry, task);
          node_executor.execute(node, task_entry);
        });
  };
  auto validate_hp_source_boundaries = [&]() {
    std::lock_guard<std::mutex> lock(graph.graph_mutex_);
    validate_hp_source_boundaries_ready(graph, dirty_plan.snapshot,
                                        hp_write_buffer);
  };

  run_dirty_source_first(
      DirtySourceFirstRunRequest{
          runtime, ComputeIntent::GlobalHighPrecision, &prepared.compute_plan,
          &prepared.selection, &prepared.source_task_ids,
          &prepared.downstream_task_ids, dirty_plan.snapshot.graph_generation,
          validate_hp_source_boundaries},
      run_hp_task);
  if (request.sibling_commit_gate) {
    request.sibling_commit_gate->wait_for_rt_commit_or_throw();
  }
  graph_lock.lock();
  hp_write_buffer.commit_to_graph(graph);
  if (!request.suppress_graph_downsample) {
    DownsampleExecutor(graph, proxy_graph, runtime, events_)
        .execute(hp_write_buffer.downsample_requests());
  }
  return require_target_output(graph, request.node_id);
}

/**
 * @brief Constructs the RT dirty executor from borrowed support services.
 *
 * @param traversal Traversal service used by dirty planning.
 * @param events Event sink used by RT node execution.
 * @throws Nothing directly.
 * @note Both services must outlive this request-scoped executor.
 */
RealTimeDirtyExecutor::RealTimeDirtyExecutor(GraphTraversalService& traversal,
                                             GraphEventService& events)
    : traversal_(traversal), events_(events) {}

/**
 * @brief Clears proxy state selected by one RT dirty plan.
 *
 * @param proxy_graph RT proxy graph whose selected nodes are reset.
 * @param plan RT dirty plan containing nodes to reset.
 * @return Nothing.
 * @throws std::bad_alloc unchanged if node-id bookkeeping exhausts memory.
 * @note Proxy graph owns synchronization for the batched reset operation.
 */
void RealTimeDirtyExecutor::reset_plan_cache(
    RealtimeProxyGraph& proxy_graph, const RealTimeDirtyPlan& plan) const {
  std::vector<int> node_ids;
  node_ids.reserve(plan.entries.size());
  for (const auto& [node_id, entry] : plan.entries) {
    (void)entry;
    node_ids.push_back(node_id);
  }
  proxy_graph.reset_nodes(node_ids);
}

/**
 * @brief Returns the committed RT target output after dirty execution.
 *
 * @param proxy_graph Proxy graph owning the RT output.
 * @param node_id Target node selected by the internal service request.
 * @return Mutable committed proxy output.
 * @throws GraphError when execution did not commit target output.
 * @throws std::bad_alloc unchanged if failure diagnostics allocate.
 * @note The returned reference remains proxy-graph-owned.
 */
NodeOutput& RealTimeDirtyExecutor::require_target_output(
    RealtimeProxyGraph& proxy_graph, int node_id) const {
  return proxy_graph.require_output(node_id);
}

/**
 * @brief Plans, executes, and commits one RT dirty request.
 *
 * @param graph Graph supplying topology, parameters, and HP fallback output.
 * @param proxy_graph RT proxy graph receiving the staged result.
 * @param runtime Optional scheduler/trace owner; null executes work inline.
 * @param request Dirty target, ROI, cache, and telemetry options.
 * @return Mutable target RT output owned by proxy_graph.
 * @throws std::bad_alloc unchanged when planning, task, proxy, staging,
 * telemetry, or output storage exhausts memory.
 * @throws GraphError for planning, dependency, operation, scheduler, commit, or
 * target validation failures.
 * @note Planning and commit hold graph_mutex_ while scheduler work runs outside
 * that lock. RT output never becomes formal reusable GraphModel cache.
 */
NodeOutput& RealTimeDirtyExecutor::execute(GraphModel& graph,
                                           RealtimeProxyGraph& proxy_graph,
                                           GraphRuntime* runtime,
                                           const DirtyUpdateRequest& request) {
  std::unique_lock<std::mutex> graph_lock(graph.graph_mutex_);

  if (request.force_recache) {
    graph.clear_full_task_graph_cache();
  }
  proxy_graph.synchronize_with_graph(graph);

  RoiPropagationService roi_propagation;
  DirtyRegionPlanner dirty_planner(traversal_, roi_propagation);
  auto prepared = prepare_dirty_execution(
      graph,
      dirty_planner.plan_real_time(graph, request.node_id, request.dirty_roi),
      ComputeRequest{ComputeIntent::RealTimeUpdate, request.node_id, false,
                     request.dirty_roi});
  RealTimeDirtyPlan& dirty_plan = prepared.dirty_plan;
  if (request.force_recache) {
    reset_plan_cache(proxy_graph, dirty_plan);
  }
  graph_lock.unlock();

  RealtimeProxyWriteBuffer rt_write_buffer(proxy_graph, !request.force_recache);
  DirtyNodeMutexMap node_mutexes =
      make_dirty_node_mutexes(prepared.compute_plan);
  DirtyNodeExecutionContext node_context{graph,
                                         runtime,
                                         events_,
                                         dirty_plan.snapshot,
                                         dirty_plan.snapshot.graph_generation,
                                         node_mutexes};
  RealTimeDirtyNodeExecutor node_executor(node_context, proxy_graph,
                                          rt_write_buffer);
  auto run_rt_task = [&](int task_id) {
    run_planned_dirty_task(
        runtime, dirty_plan.entries, prepared.compute_plan, task_id,
        [&](int node_id, RtPlanEntry& entry, const PlannedTask& task) {
          Node& node = graph.mutable_node(node_id);
          RtPlanEntry task_entry = entry_for_task(entry, task);
          node_executor.execute(node, task_entry);
        });
  };
  auto validate_rt_source_boundaries = [&]() {
    std::lock_guard<std::mutex> lock(graph.graph_mutex_);
    validate_rt_source_boundaries_ready(graph, proxy_graph, dirty_plan.snapshot,
                                        rt_write_buffer);
  };

  run_dirty_source_first(
      DirtySourceFirstRunRequest{
          runtime, ComputeIntent::RealTimeUpdate, &prepared.compute_plan,
          &prepared.selection, &prepared.source_task_ids,
          &prepared.downstream_task_ids, dirty_plan.snapshot.graph_generation,
          validate_rt_source_boundaries},
      run_rt_task);
  rt_write_buffer.commit_to_proxy_graph();
  return require_target_output(proxy_graph, request.node_id);
}

}  // namespace ps::compute
