#include "kernel/services/compute-service/dirty_update_executor.hpp"

#include <algorithm>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/graph_runtime.hpp"
#include "kernel/services/compute-service/compute_cache_policy.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"
#include "kernel/services/compute-service/compute_task_dispatcher.hpp"
#include "kernel/services/compute-service/dirty_region_planner.hpp"
#include "kernel/services/compute-service/node_executor.hpp"
#include "kernel/services/compute-service/node_input_resolver.hpp"
#include "kernel/services/compute-service/task_graph_planning.hpp"
#include "kernel/services/graph_event_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"
#include "kernel/services/roi_propagation_service.hpp"

namespace ps::compute {
namespace {

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
 * @brief Stores the latest dirty snapshot and bounded history on the graph.
 *
 * @param graph Graph whose inspection state receives the snapshot.
 * @param snapshot Dirty-region snapshot generated for the current request.
 * @throws std::bad_alloc if snapshot history storage cannot grow.
 * @note The history cap mirrors ComputeService's existing inspection policy.
 */
void remember_dirty_snapshot(GraphModel& graph,
                             const DirtyRegionSnapshot& snapshot) {
  graph.last_dirty_region_snapshot = snapshot;
  graph.recent_dirty_region_snapshots.push_back(snapshot);
  if (graph.recent_dirty_region_snapshots.size() > 16) {
    graph.recent_dirty_region_snapshots.erase(
        graph.recent_dirty_region_snapshots.begin());
  }
}

/**
 * @brief Stores the latest compute plan and bounded recent-plan history.
 *
 * @param graph Graph whose inspection state receives the compute plan.
 * @param compute_plan Dirty-pruned plan for the current request.
 * @throws std::bad_alloc if plan history storage cannot grow.
 * @note Plans are diagnostic state, not an unbounded runtime trace.
 */
void remember_compute_plan(GraphModel& graph, const ComputePlan& compute_plan) {
  graph.last_compute_plan = compute_plan;
  graph.recent_compute_plans.push_back(compute_plan);
  if (graph.recent_compute_plans.size() > 16) {
    graph.recent_compute_plans.erase(graph.recent_compute_plans.begin());
  }
}

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
    const std::vector<int>& execution_order) {
  FullTaskGraphExpander full_expander;
  NodeCacheTaskGraphPruner node_cache_pruner;
  const FullTaskGraph full_graph = full_expander.expand(graph, request.intent);
  return node_cache_pruner.prune(full_graph, request, execution_order, graph);
}

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
    const ComputePlan& node_cache_plan, const DirtyRegionSnapshot& snapshot) {
  DirtySnapshotTaskGraphPruner dirty_snapshot_pruner;
  return dirty_snapshot_pruner.prune(node_cache_plan, snapshot);
}

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
                                            const std::vector<int>& task_ids) {
  std::unordered_set<int> selected_task_ids(task_ids.begin(), task_ids.end());
  std::vector<int> node_ids;
  std::unordered_set<int> selected_node_ids;
  for (const auto& work : compute_plan.planned_work) {
    bool selected = false;
    for (int task_id : work.task_ids) {
      if (selected_task_ids.count(task_id)) {
        selected = true;
        break;
      }
    }
    if (selected && selected_node_ids.insert(work.node_id).second) {
      node_ids.push_back(work.node_id);
    }
  }
  return node_ids;
}

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
                                            DirtyDomain domain) {
  for (int source_node_id : snapshot.dirty_source_nodes) {
    const Node* source = graph.find_node(source_node_id);
    if (!source) {
      throw GraphError(GraphErrc::NotFound, "Dirty source node " +
                                                std::to_string(source_node_id) +
                                                " not found.");
    }
    const NodeOutput* output =
        domain == DirtyDomain::RealTime
            ? ComputeCachePolicy::interactive_output(*source)
            : ComputeCachePolicy::reusable_output(*source);
    if (!output) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Dirty source boundary output is not ready for node " +
                           std::to_string(source_node_id) + ".");
    }
  }
}

/**
 * @brief Checks whether a node is listed as a dirty source in a snapshot.
 *
 * @param snapshot Dirty snapshot for the current request.
 * @param node_id Node id being inspected.
 * @return True when the node is a dirty source boundary.
 * @throws Nothing directly.
 * @note Source membership controls stale-generation checks and trace labels.
 */
bool is_dirty_source_node(const DirtyRegionSnapshot& snapshot, int node_id) {
  return std::find(snapshot.dirty_source_nodes.begin(),
                   snapshot.dirty_source_nodes.end(),
                   node_id) != snapshot.dirty_source_nodes.end();
}

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
                              bool dirty_source) {
  if (!runtime) {
    return;
  }
  runtime->log_event(GraphRuntime::SchedulerEvent::EXECUTE, node_id);
  runtime->log_event(
      dirty_source
          ? GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_SOURCE
          : GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_NODE,
      node_id);
}

/**
 * @brief Logs and skips stale dirty source generations.
 *
 * @param runtime Optional runtime for scheduler trace events.
 * @param node_id Dirty source node id.
 * @param committed_generation Generation already committed for this source.
 * @param dirty_generation Generation being executed by the current request.
 * @return True when the current request should skip the node.
 * @throws Any exception propagated by GraphRuntime::log_event.
 * @note The comparison intentionally preserves the previous strict-greater
 * policy so repeated execution of the same generation is still allowed.
 */
bool should_skip_stale_dirty_source(GraphRuntime* runtime, int node_id,
                                    uint64_t committed_generation,
                                    uint64_t dirty_generation) {
  if (committed_generation <= dirty_generation) {
    return false;
  }
  if (runtime) {
    runtime->log_event(GraphRuntime::SchedulerEvent::SKIP_STALE_GENERATION,
                       node_id);
  }
  return true;
}

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
    const std::optional<NodeOutput>* fallback = nullptr) {
  if (preferred) {
    const auto& buffer = preferred->image_buffer;
    if (buffer.width > 0 && buffer.height > 0 && buffer.channels > 0) {
      return {buffer.channels, buffer.type};
    }
  }
  for (const auto* input : image_inputs) {
    const auto& buffer = input->image_buffer;
    if (buffer.width > 0 && buffer.height > 0 && buffer.channels > 0) {
      return {buffer.channels, buffer.type};
    }
  }
  if (fallback && *fallback) {
    const auto& buffer = (*fallback)->image_buffer;
    if (buffer.width > 0 && buffer.height > 0 && buffer.channels > 0) {
      return {buffer.channels, buffer.type};
    }
  }
  return {1, DataType::FLOAT32};
}

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
                             const ComputePlan& compute_plan) {
  for (const auto& work : compute_plan.planned_work) {
    auto entry_it = entries.find(work.node_id);
    if (entry_it == entries.end()) {
      continue;
    }
    if (!is_rect_empty(work.represented_hp_roi)) {
      entry_it->second.roi_hp =
          clip_rect(work.represented_hp_roi, entry_it->second.hp_size);
    }
  }
}

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
                             const ComputePlan& compute_plan) {
  for (const auto& work : compute_plan.planned_work) {
    auto entry_it = entries.find(work.node_id);
    if (entry_it == entries.end()) {
      continue;
    }
    if (!is_rect_empty(work.represented_hp_roi)) {
      entry_it->second.roi_hp =
          clip_rect(work.represented_hp_roi, entry_it->second.hp_size);
    }
    if (!is_rect_empty(work.execution_roi)) {
      entry_it->second.roi_rt =
          clip_rect(work.execution_roi, entry_it->second.rt_size);
    }
  }
}

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
 * @param runtime Optional runtime. When present, tasks are submitted to the
 * intent-specific scheduler runtime; otherwise they run inline.
 * @param intent Compute intent used for scheduler runtime lookup.
 * @param source_node_ids Source boundary nodes selected by dirty materialize.
 * @param downstream_node_ids Dependent dirty nodes selected by materialize.
 * @param dirty_generation Dirty snapshot generation used as scheduler epoch.
 * @param make_task Factory for node execution closures.
 * @param before_downstream Boundary validation callback.
 * @throws Exceptions from task construction, task execution, boundary
 * validation, scheduler lookup, or scheduler submission.
 * @note Ordering intentionally mirrors the pre-split ComputeService logic:
 * all source tasks finish before downstream tasks are released.
 */
template <typename MakeTask>
void run_dirty_source_first(GraphRuntime* runtime, ComputeIntent intent,
                            const std::vector<int>& source_node_ids,
                            const std::vector<int>& downstream_node_ids,
                            uint64_t dirty_generation, MakeTask make_task,
                            std::function<void()> before_downstream) {
  if (runtime) {
    SchedulerTaskRuntime& dirty_task_runtime =
        ensure_scheduler_task_runtime(*runtime, intent);
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
        std::move(downstream_tasks), dirty_generation,
        std::move(before_downstream));
    return;
  }

  for (int source_node_id : source_node_ids) {
    make_task(source_node_id)();
  }
  if (before_downstream) {
    before_downstream();
  }
  for (int downstream_node_id : downstream_node_ids) {
    make_task(downstream_node_id)();
  }
}

}  // namespace

SchedulerTaskRuntime& ensure_scheduler_task_runtime(GraphRuntime& runtime,
                                                    ComputeIntent intent) {
  IScheduler* scheduler = runtime.get_scheduler(intent);
  if (!scheduler) {
    throw GraphError(GraphErrc::ComputeError,
                     "No scheduler registered for requested compute intent.");
  }
  auto* task_runtime = dynamic_cast<SchedulerTaskRuntime*>(scheduler);
  if (!task_runtime) {
    throw GraphError(GraphErrc::ComputeError,
                     "Registered scheduler cannot dispatch planned tasks.");
  }
  if (!task_runtime->task_runtime_running()) {
    scheduler->start();
  }
  return *task_runtime;
}

DownsampleExecutor::DownsampleExecutor(GraphModel& graph, GraphRuntime* runtime,
                                       GraphEventService& events)
    : graph_(graph), runtime_(runtime), events_(events) {}

void DownsampleExecutor::execute(const std::vector<Request>& requests) {
  for (const auto& request : requests) {
    execute_one(request);
  }
}

void DownsampleExecutor::execute_one(const Request& request) {
  Node* node_ptr = graph_.find_node_mutable(request.node_id);
  if (!node_ptr) {
    return;
  }
  Node& node = *node_ptr;
  if (!node.cached_output_high_precision) {
    return;
  }
  if (node.hp_version < request.hp_version) {
    if (runtime_) {
      runtime_->log_event(GraphRuntime::SchedulerEvent::SKIP_STALE_GENERATION,
                          node.id);
    }
    return;
  }
  if (node.rt_version > request.hp_version) {
    if (runtime_) {
      runtime_->log_event(GraphRuntime::SchedulerEvent::SKIP_STALE_GENERATION,
                          node.id);
    }
    return;
  }

  const NodeOutput& hp_output = *node.cached_output_high_precision;
  const ImageBuffer& hp_buffer = hp_output.image_buffer;
  cv::Size hp_size(std::max(hp_buffer.width, 0), std::max(hp_buffer.height, 0));
  cv::Rect roi_hp = clip_rect(request.roi_hp, hp_size);
  if (is_rect_empty(roi_hp) && hp_size.width > 0 && hp_size.height > 0) {
    roi_hp = cv::Rect(0, 0, hp_size.width, hp_size.height);
  }

  if (!node.cached_output_real_time) {
    node.cached_output_real_time = NodeOutput{};
  }
  node.cached_output_real_time->data = hp_output.data;

  if (hp_buffer.width <= 0 || hp_buffer.height <= 0 || !hp_buffer.data) {
    node.cached_output_real_time = node.cached_output_high_precision;
    if (!is_rect_empty(roi_hp)) {
      node.rt_roi = node.rt_roi.has_value()
                        ? clip_rect(merge_rect(*node.rt_roi, roi_hp), hp_size)
                        : roi_hp;
    }
    node.rt_version = request.hp_version;
    events_.push(node.id, node.name, "downsample_passthrough", 0.0);
    return;
  }

  cv::Size rt_size = scale_down_size(hp_size, kRtDownscaleFactor);
  if (rt_size.width <= 0 || rt_size.height <= 0) {
    node.cached_output_real_time = node.cached_output_high_precision;
    if (!is_rect_empty(roi_hp)) {
      node.rt_roi = node.rt_roi.has_value()
                        ? clip_rect(merge_rect(*node.rt_roi, roi_hp), hp_size)
                        : roi_hp;
    }
    node.rt_version = request.hp_version;
    events_.push(node.id, node.name, "downsample_passthrough", 0.0);
    return;
  }

  ImageBuffer& rt_buffer = node.cached_output_real_time->image_buffer;
  const bool needs_alloc = (rt_buffer.width != rt_size.width) ||
                           (rt_buffer.height != rt_size.height) ||
                           (rt_buffer.channels != hp_buffer.channels) ||
                           (rt_buffer.type != hp_buffer.type) ||
                           (!rt_buffer.data);
  if (needs_alloc) {
    rt_buffer = make_aligned_cpu_image_buffer(
        rt_size.width, rt_size.height, hp_buffer.channels, hp_buffer.type);
  }

  cv::Rect roi_rt =
      clip_rect(scale_down_rect(roi_hp, kRtDownscaleFactor), rt_size);
  if (is_rect_empty(roi_rt)) {
    roi_rt = cv::Rect(0, 0, rt_size.width, rt_size.height);
  }

  cv::Mat hp_mat = toCvMat(hp_buffer);
  cv::Mat rt_mat = toCvMat(rt_buffer);
  cv::Mat hp_patch = hp_mat(roi_hp);
  cv::Mat downsampled;
  cv::resize(hp_patch, downsampled, cv::Size(roi_rt.width, roi_rt.height), 0, 0,
             cv::INTER_LINEAR);
  downsampled.copyTo(rt_mat(roi_rt));

  node.rt_roi = node.rt_roi.has_value()
                    ? clip_rect(merge_rect(*node.rt_roi, roi_hp), hp_size)
                    : roi_hp;
  node.rt_version = request.hp_version;
  events_.push(node.id, node.name, "downsample", 0.0);

  if (runtime_) {
    runtime_->log_event(GraphRuntime::SchedulerEvent::EXECUTE_TILE, node.id);
  }
}

HighPrecisionDirtyExecutor::HighPrecisionDirtyExecutor(
    GraphTraversalService& traversal, GraphEventService& events)
    : traversal_(traversal), events_(events) {}

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

  auto& plan = prepared.dirty_plan.entries;
  const uint64_t dirty_generation =
      prepared.dirty_plan.snapshot.graph_generation;
  std::vector<DownsampleExecutor::Request> downsample_requests;
  std::mutex downsample_requests_mutex;

  if (request.force_recache) {
    for (const auto& [nid, _] : plan) {
      Node& node = graph.mutable_node(nid);
      node.cached_output_high_precision.reset();
      node.hp_roi.reset();
      node.hp_version = 0;
    }
  }

  auto compute_node_hp = [&](int nid, HpPlanEntry& entry) {
    if (is_rect_empty(entry.roi_hp)) {
      return;
    }
    Node& node = graph.mutable_node(nid);
    const bool dirty_source =
        is_dirty_source_node(prepared.dirty_plan.snapshot, nid);
    if (dirty_source &&
        should_skip_stale_dirty_source(
            runtime, nid, graph.dirty_source_hp_commit_generation[nid],
            dirty_generation)) {
      return;
    }

    log_dirty_node_execution(runtime, nid, dirty_source);

    auto resolved_inputs = NodeInputResolver::resolve(
        node,
        [&](int upstream_id) -> const NodeOutput* {
          const Node* upstream = graph.find_node(upstream_id);
          if (!upstream) {
            return nullptr;
          }
          return ComputeCachePolicy::reusable_output(*upstream);
        },
        "HP update");
    const auto& image_inputs_ready = resolved_inputs.image_inputs;

    const auto* impls =
        OpRegistry::instance().get_implementations(node.type, node.subtype);
    const TileOpFunc* hp_tile_fn =
        (impls && impls->tiled_hp) ? &*impls->tiled_hp : nullptr;
    const MonolithicOpFunc* hp_mono_fn =
        (impls && impls->monolithic_hp) ? &*impls->monolithic_hp : nullptr;

    if (hp_tile_fn) {
      auto [channels, dtype] = infer_output_spec(
          node.cached_output_high_precision, image_inputs_ready);
      if (!node.cached_output_high_precision) {
        node.cached_output_high_precision = NodeOutput{};
      }
      ImageBuffer& hp_buffer = node.cached_output_high_precision->image_buffer;
      if (hp_buffer.width != entry.hp_size.width ||
          hp_buffer.height != entry.hp_size.height ||
          hp_buffer.channels != channels || hp_buffer.type != dtype ||
          !hp_buffer.data) {
        hp_buffer = make_aligned_cpu_image_buffer(
            entry.hp_size.width, entry.hp_size.height, channels, dtype);
      }

      TiledExecutionConfig config;
      config.tile_size = kHpMicroTileSize;
      config.output_roi = entry.roi_hp;
      config.output_size = entry.hp_size;
      config.forced_halo = entry.halo_hp;
      if (auto metadata =
              OpRegistry::instance().get_metadata(node.type, node.subtype)) {
        config.metadata = *metadata;
      }
      if (runtime) {
        runtime->log_event(GraphRuntime::SchedulerEvent::EXECUTE_TILE, nid);
        runtime->log_event(
            GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_TILE, nid);
      }
      NodeExecutor::execute_tiled_into(graph, node, *hp_tile_fn,
                                       image_inputs_ready, hp_buffer, config);
    } else if (hp_mono_fn) {
      node.cached_output_high_precision =
          (*hp_mono_fn)(node, image_inputs_ready);

      if (!node.cached_output_high_precision) {
        throw GraphError(GraphErrc::ComputeError,
                         "Monolithic HP operator produced no output for " +
                             node.type + ":" + node.subtype);
      }
    } else {
      throw GraphError(GraphErrc::NoOperation,
                       "No suitable HP operator (tiled or monolithic) for " +
                           node.type + ":" + node.subtype);
    }

    node.hp_roi =
        node.hp_roi.has_value()
            ? clip_rect(merge_rect(*node.hp_roi, entry.roi_hp), entry.hp_size)
            : entry.roi_hp;
    node.hp_version++;
    if (dirty_source) {
      graph.dirty_source_hp_commit_generation[nid] = dirty_generation;
    }
    events_.push(node.id, node.name, "hp_update", 0.0);

    if (runtime) {
      std::lock_guard<std::mutex> lock(downsample_requests_mutex);
      downsample_requests.push_back({node.id, entry.roi_hp, node.hp_version});
    }
  };

  auto make_hp_task = [&](int task_node_id) -> SchedulerTaskRuntime::Task {
    return [&, task_node_id]() {
      auto entry_it = plan.find(task_node_id);
      if (entry_it == plan.end()) {
        return;
      }
      try {
        compute_node_hp(task_node_id, entry_it->second);
      } catch (...) {
        if (runtime) {
          runtime->log_event(GraphRuntime::SchedulerEvent::RETHROW_EXCEPTION,
                             task_node_id);
        }
        throw;
      }
    };
  };

  auto validate_hp_source_boundaries = [&]() {
    validate_dirty_source_boundaries_ready(graph, prepared.dirty_plan.snapshot,
                                           DirtyDomain::HighPrecision);
  };

  run_dirty_source_first(runtime, ComputeIntent::GlobalHighPrecision,
                         prepared.source_node_ids, prepared.downstream_node_ids,
                         dirty_generation, make_hp_task,
                         validate_hp_source_boundaries);

  DownsampleExecutor(graph, runtime, events_).execute(downsample_requests);

  Node& target = graph.mutable_node(request.node_id);
  if (!target.cached_output_high_precision) {
    throw GraphError(GraphErrc::ComputeError,
                     "HP compute finished without target output.");
  }
  return *target.cached_output_high_precision;
}

RealTimeDirtyExecutor::RealTimeDirtyExecutor(GraphTraversalService& traversal,
                                             GraphEventService& events)
    : traversal_(traversal), events_(events) {}

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

  auto& plan = prepared.dirty_plan.entries;
  const uint64_t dirty_generation =
      prepared.dirty_plan.snapshot.graph_generation;

  if (request.force_recache) {
    for (const auto& [nid, _] : plan) {
      Node& node = graph.mutable_node(nid);
      node.cached_output_real_time.reset();
      node.rt_roi.reset();
      node.rt_version = 0;
    }
  }

  auto compute_node_rt = [&](int nid, RtPlanEntry& entry) {
    Node& node = graph.mutable_node(nid);
    if (is_rect_empty(entry.roi_rt)) {
      return;
    }
    const bool dirty_source =
        is_dirty_source_node(prepared.dirty_plan.snapshot, nid);
    if (dirty_source &&
        should_skip_stale_dirty_source(
            runtime, nid, graph.dirty_source_rt_commit_generation[nid],
            dirty_generation)) {
      return;
    }

    log_dirty_node_execution(runtime, nid, dirty_source);

    auto resolved_inputs = NodeInputResolver::resolve(
        node,
        [&](int upstream_id) -> const NodeOutput* {
          const Node* upstream = graph.find_node(upstream_id);
          if (!upstream) {
            return nullptr;
          }
          return ComputeCachePolicy::interactive_output(*upstream);
        },
        "RT update");
    const auto& image_inputs_ready = resolved_inputs.image_inputs;

    auto op_variant = OpRegistry::instance().resolve_for_intent(
        node.type, node.subtype, ComputeIntent::RealTimeUpdate);
    if (!op_variant) {
      op_variant = OpRegistry::instance().resolve_for_intent(
          node.type, node.subtype, ComputeIntent::GlobalHighPrecision);
    }
    if (!op_variant) {
      throw GraphError(
          GraphErrc::NoOperation,
          "No operator registered for node " + node.type + ":" + node.subtype);
    }

    auto [channels, dtype] =
        infer_output_spec(node.cached_output_real_time, image_inputs_ready,
                          &node.cached_output_high_precision);
    if (!node.cached_output_real_time) {
      node.cached_output_real_time = NodeOutput{};
    }
    ImageBuffer& rt_buffer = node.cached_output_real_time->image_buffer;
    bool needs_alloc = (rt_buffer.width != entry.rt_size.width) ||
                       (rt_buffer.height != entry.rt_size.height) ||
                       (rt_buffer.channels != channels) ||
                       (rt_buffer.type != dtype) || (!rt_buffer.data);
    if (needs_alloc) {
      rt_buffer = make_aligned_cpu_image_buffer(
          entry.rt_size.width, entry.rt_size.height, channels, dtype);
    }

    try {
      std::visit(
          [&](auto&& fn) {
            using T = std::decay_t<decltype(fn)>;
            if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
              NodeOutput result = fn(node, image_inputs_ready);
              if (result.image_buffer.width > 0 &&
                  result.image_buffer.height > 0) {
                cv::Mat result_mat = toCvMat(result.image_buffer);
                if (rt_buffer.width != entry.rt_size.width ||
                    rt_buffer.height != entry.rt_size.height ||
                    rt_buffer.channels != result.image_buffer.channels ||
                    rt_buffer.type != result.image_buffer.type ||
                    !rt_buffer.data) {
                  rt_buffer = make_aligned_cpu_image_buffer(
                      entry.rt_size.width, entry.rt_size.height,
                      result.image_buffer.channels, result.image_buffer.type);
                }
                if (result_mat.cols != entry.rt_size.width ||
                    result_mat.rows != entry.rt_size.height) {
                  cv::resize(
                      result_mat, result_mat,
                      cv::Size(entry.rt_size.width, entry.rt_size.height), 0, 0,
                      cv::INTER_LINEAR);
                }
                cv::Mat dest = toCvMat(rt_buffer);
                result_mat(entry.roi_rt).copyTo(dest(entry.roi_rt));
              }
              node.cached_output_real_time->data = result.data;
            } else if constexpr (std::is_same_v<T, TileOpFunc>) {
              TiledExecutionConfig config;
              config.tile_size = kRtTileSize;
              config.output_roi = entry.roi_rt;
              config.output_size = entry.rt_size;
              config.forced_halo = entry.halo_rt;
              if (auto metadata = OpRegistry::instance().get_metadata(
                      node.type, node.subtype)) {
                config.metadata = *metadata;
              }
              NodeExecutor::execute_tiled_into(
                  graph, node, fn, image_inputs_ready, rt_buffer, config);
              if (runtime) {
                runtime->log_event(GraphRuntime::SchedulerEvent::EXECUTE_TILE,
                                   nid);
                runtime->log_event(
                    GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_TILE,
                    nid);
              }
            }
          },
          *op_variant);
    } catch (const cv::Exception& e) {
      throw GraphError(GraphErrc::ComputeError, "RT compute failed at node " +
                                                    std::to_string(nid) + ": " +
                                                    std::string(e.what()));
    } catch (const GraphError&) {
      throw;
    } catch (const std::exception& e) {
      throw GraphError(GraphErrc::ComputeError, "RT compute failed at node " +
                                                    std::to_string(nid) + ": " +
                                                    std::string(e.what()));
    }

    if (node.rt_roi.has_value()) {
      node.rt_roi =
          clip_rect(merge_rect(*node.rt_roi, entry.roi_hp), entry.hp_size);
    } else {
      node.rt_roi = entry.roi_hp;
    }
    node.rt_version++;
    if (dirty_source) {
      graph.dirty_source_rt_commit_generation[nid] = dirty_generation;
    }
    events_.push(node.id, node.name, "rt_update", 0.0);
  };

  auto make_rt_task = [&](int task_node_id) -> SchedulerTaskRuntime::Task {
    return [&, task_node_id]() {
      auto entry_it = plan.find(task_node_id);
      if (entry_it == plan.end()) {
        return;
      }
      try {
        compute_node_rt(task_node_id, entry_it->second);
      } catch (...) {
        if (runtime) {
          runtime->log_event(GraphRuntime::SchedulerEvent::RETHROW_EXCEPTION,
                             task_node_id);
        }
        throw;
      }
    };
  };

  auto validate_rt_source_boundaries = [&]() {
    validate_dirty_source_boundaries_ready(graph, prepared.dirty_plan.snapshot,
                                           DirtyDomain::RealTime);
  };

  run_dirty_source_first(runtime, ComputeIntent::RealTimeUpdate,
                         prepared.source_node_ids, prepared.downstream_node_ids,
                         dirty_generation, make_rt_task,
                         validate_rt_source_boundaries);

  Node& target = graph.mutable_node(request.node_id);
  if (!target.cached_output_real_time) {
    throw GraphError(GraphErrc::ComputeError,
                     "RT compute finished without target output.");
  }
  return *target.cached_output_real_time;
}

}  // namespace ps::compute
