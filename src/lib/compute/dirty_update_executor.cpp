#include "compute/dirty_update_executor.hpp"

#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "compute/compute_cache_policy.hpp"
#include "compute/compute_geometry.hpp"
#include "compute/compute_run.hpp"
#include "compute/dirty_execution_common.hpp"
#include "compute/dirty_node_executor.hpp"
#include "compute/dirty_region_planner.hpp"
#include "compute/dirty_sibling_commit_gate.hpp"
#include "compute/dirty_write_buffers.hpp"
#include "compute/downsample_executor.hpp"
#include "compute/node_executor.hpp"
#include "compute/node_input_resolver.hpp"
#include "compute/realtime_proxy_graph.hpp"
#include "compute/resource_demand_estimator.hpp"
#include "graph/graph_extent_resolver.hpp"
#include "graph/graph_traversal_service.hpp"
#include "graph/roi_propagation_service.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/graph_runtime.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Rejects a dirty/preflight boundary after accepted Run cancellation.
 * @param run Optional request observer for inline/legacy callers.
 * @param run_lease Preferred retained lifecycle lease for product callers.
 * @return Nothing while no explicit/deadline cancellation has won.
 * @throws GraphError with ComputeError after cancellation.
 * @throws std::system_error when Run-state synchronization fails.
 * @note The Run retains the stable reason for outer ComputeService
 * translation; this helper only terminates later dirty/preflight work.
 */
void observe_dirty_run_or_throw(ComputeRun* run,
                                const ComputeRunLease* run_lease) {
  std::optional<ComputeRunCancellationReason> reason;
  if (run_lease != nullptr) {
    reason = run_lease->observe_cancellation();
  } else if (run != nullptr) {
    reason = run->observe_cancellation();
  }
  if (reason.has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ComputeRun cancelled during dirty execution.");
  }
}

/**
 * @brief Advances an optional dirty-domain Run to executable phase.
 *
 * @param run Child or standalone Run, or null for legacy callers.
 * @param run_lease Preferred borrowed lifecycle lease for product callers.
 * @param queued Whether work crosses a scheduler/service queue.
 * @return Nothing.
 * @throws std::logic_error when the Run was not admitted or has already
 * reached commit/terminal state.
 * @throws std::invalid_argument from invalid phase transitions.
 * @note Repeated calls while Running are idempotent so connected-parameter
 * preflight and the following dirty phase may share one domain Run.
 * Cancellation is observed before transition and after every transition that
 * could otherwise admit provider work.
 */
void advance_dirty_run_for_execution(ComputeRun* run,
                                     const ComputeRunLease* run_lease,
                                     bool queued) {
  if (!run) {
    return;
  }
  observe_dirty_run_or_throw(run, run_lease);
  switch (run->phase()) {
    case ComputeRunPhase::Created:
      throw std::logic_error(
          "Dirty execution requires an admitted ComputeRun.");
    case ComputeRunPhase::Admitted:
      if (queued) {
        run->advance_to(ComputeRunPhase::Queued);
      }
      run->advance_to(ComputeRunPhase::Running);
      observe_dirty_run_or_throw(run, run_lease);
      return;
    case ComputeRunPhase::Queued:
      run->advance_to(ComputeRunPhase::Running);
      observe_dirty_run_or_throw(run, run_lease);
      return;
    case ComputeRunPhase::Running:
      return;
    case ComputeRunPhase::CommitPending:
    case ComputeRunPhase::Terminal:
      throw std::logic_error(
          "Dirty execution cannot reuse a settled or committing ComputeRun.");
  }
  throw std::logic_error("Dirty execution observed an unknown Run phase.");
}

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
 * @brief Builds synchronization for graph nodes present in a dirty plan.
 *
 * @param compute_plan Node/cache-pruned plan whose planned work will execute.
 * @return Shared owner of per-node snapshot and staging critical sections.
 * @throws std::bad_alloc if mutex allocation fails.
 * @note The returned owner remains local unless ComputeService supplied one
 * transaction-wide object to both HP and RT siblings. It owns no scheduler
 * state, output buffer, or commit policy.
 */
std::shared_ptr<DirtyNodeSynchronization> make_dirty_node_synchronization(
    const ComputePlan& compute_plan) {
  std::vector<int> node_ids;
  node_ids.reserve(compute_plan.planned_work.size());
  for (const auto& work : compute_plan.planned_work) {
    node_ids.push_back(work.node_id);
  }
  return std::make_shared<DirtyNodeSynchronization>(node_ids);
}

/**
 * @brief Exposes one connected-parameter preflight callback as a scheduler
 * task handle.
 *
 * @tparam RunTask Request-local callback type.
 * @throws Nothing during construction; run_task() documents invocation and
 * scheduler-publication exceptions.
 * @note The executor borrows its callback and scheduler runtime. It must remain
 * alive until the matching wait_for_completion() call returns or throws.
 */
template <typename RunTask>
class PreflightTaskExecutor final : public TaskExecutor {
 public:
  /**
   * @brief Binds one preflight callback to its scheduler completion contract.
   * @param run_task Callback that computes and stages one producer node.
   * @param task_runtime Runtime receiving trace and completion publication.
   * @param node_id Graph node id used in scheduler trace events.
   * @throws Nothing.
   */
  PreflightTaskExecutor(RunTask& run_task, SchedulerTaskRuntime& task_runtime,
                        int node_id)
      : run_task_(run_task), task_runtime_(task_runtime), node_id_(node_id) {}

  /**
   * @brief Executes the sole preflight task and settles scheduler accounting.
   * @param task_id Must be zero for this single-task executor.
   * @return Nothing.
   * @throws GraphError when task_id is invalid.
   * @throws The exact callback, trace, or completion-publication exception.
   * @note Rethrow tracing is best-effort so a hostile scheduler log hook cannot
   * replace the authoritative task exception or prevent batch completion.
   */
  void run_task(int task_id) override {
    if (task_id != 0) {
      throw GraphError(GraphErrc::ComputeError,
                       "Connected-parameter preflight task id is invalid");
    }
    try {
      task_runtime_.log_event(SchedulerTraceAction::Execute, node_id_);
      run_task_();
      task_runtime_.dec_tasks_to_complete();
    } catch (...) {
      try {
        task_runtime_.log_event(SchedulerTraceAction::RethrowException,
                                node_id_);
      } catch (...) {
      }
      throw;
    }
  }

 private:
  /** @brief Borrowed request-local preflight callback. */
  RunTask& run_task_;
  /** @brief Borrowed scheduler runtime for the active initial batch. */
  SchedulerTaskRuntime& task_runtime_;
  /** @brief Node id reported in scheduler trace events. */
  int node_id_ = -1;
};

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
PixelRect hp_planning_roi_for_request(const GraphModel& graph,
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
      return PixelRect{0, 0, buffer.width, buffer.height};
    }
  }

  GraphExtentResolver extent_resolver;
  std::unordered_map<int, PixelSize> extent_cache;
  const PixelSize target_extent = extent_resolver.resolve_output_extent(
      graph, request.node_id, extent_cache);
  if (target_extent.width <= 0 || target_extent.height <= 0) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Cannot compute forced HP dirty update for node " +
                         std::to_string(request.node_id) +
                         ": HP output extent is unavailable.");
  }
  return PixelRect{0, 0, target_extent.width, target_extent.height};
}

/**
 * @brief Reports whether an output carries an executable image payload.
 * @param output Output produced by a parameter stabilization operation.
 * @return True when positive image dimensions and storage ownership coexist.
 * @throws Nothing.
 * @note Metadata-only dimensions are treated as image-carrying conservatively
 * when either data or context retains the payload.
 */
bool has_image_payload(const NodeOutput& output) noexcept {
  const ImageBuffer& image = output.image_buffer;
  return image.width > 0 && image.height > 0 &&
         (image.data != nullptr || image.context != nullptr);
}

/**
 * @brief Builds exact execution-local parameters from a stabilized map.
 * @param node Node whose static parameters and bindings are merged.
 * @param graph Live graph supplying unaffected committed parameter outputs.
 * @param stabilized Immutable preflight parameter producer outputs.
 * @return Deep-owned effective ParameterMap.
 * @throws GraphError when a producer or named data output is unavailable.
 * @throws std::bad_alloc from recursive value copying.
 * @note Image payload selection is deliberately absent from this helper.
 */
plugin::ParameterMap stabilized_runtime_parameters(
    const Node& node, const GraphModel& graph,
    const StabilizedDirtyParameters& stabilized) {
  plugin::ParameterMap effective = node.parameters;
  for (const ParameterInput& input : node.parameter_inputs) {
    if (input.from_node_id < 0) {
      continue;
    }
    const NodeOutput* output =
        stabilized.find_parameter_output(input.from_node_id);
    if (!output) {
      const Node* producer = graph.find_node(input.from_node_id);
      output =
          producer ? ComputeCachePolicy::reusable_output(*producer) : nullptr;
    }
    if (!output) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Stabilized parameter input not ready for node " +
                           std::to_string(node.id));
    }
    const auto value = output->data.find(input.from_output_name);
    if (value == output->data.end()) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Node " + std::to_string(input.from_node_id) +
                           " did not produce output '" +
                           input.from_output_name + "'");
    }
    effective.insert_or_assign(input.to_parameter_name, value->second);
  }
  return effective;
}

/**
 * @brief Clones live topology into a request-local stabilized planning graph.
 *
 * @param graph Live graph supplying topology and unaffected cache snapshots.
 * @param stabilized Immutable parameter stabilization result.
 * @param hp_domain Whether all preflight closure outputs should become shadow
 * HP cache; RT injects only direct parameter producer values.
 * @return Independent graph used only for extent/task/dirty planning.
 * @throws GraphError or std::bad_alloc from node cloning and graph validation.
 * @note Geometry-affected caches are cleared before stabilized outputs are
 * installed. The shadow has its own FullTaskGraph cache and therefore cannot
 * reuse a stale live-graph task expansion.
 */
std::unique_ptr<GraphModel> make_stabilized_planning_graph(
    const GraphModel& graph, const StabilizedDirtyParameters& stabilized,
    bool hp_domain) {
  GraphModel::NodeMap nodes;
  nodes.reserve(graph.node_count());
  for (int node_id : graph.node_ids()) {
    Node node = graph.node(node_id);
    if (stabilized.geometry_affected(node_id)) {
      node.cached_output_high_precision.reset();
      node.hp_roi.reset();
      node.runtime_parameters =
          stabilized_runtime_parameters(node, graph, stabilized);
    }
    nodes.emplace(node_id, std::move(node));
  }

  for (const auto& [node_id, staged] : stabilized.staged_outputs()) {
    if (!hp_domain &&
        !stabilized.parameter_producer_node_ids().count(node_id)) {
      continue;
    }
    auto node_it = nodes.find(node_id);
    if (node_it == nodes.end()) {
      throw GraphError(GraphErrc::NotFound, "Stabilized planning node " +
                                                std::to_string(node_id) +
                                                " is missing.");
    }
    node_it->second.cached_output_high_precision = staged.output;
    node_it->second.hp_version = staged.hp_version;
    node_it->second.hp_roi = staged.hp_roi;
  }

  auto planning_graph = std::make_unique<GraphModel>(graph.cache_root);
  planning_graph->replace_nodes(std::move(nodes));
  planning_graph->dirty_generation_counter = graph.dirty_generation_counter;
  return planning_graph;
}

/**
 * @brief Estimates request-local dirty planning storage retained by callbacks.
 * @tparam DirtyPlan HP or RT dirty-plan type.
 * @param prepared Prepared plan whose original values remain live while the
 * service-owned context executes its separate copies.
 * @return Checked complete dirty plan, compute plan, overlay, and work-set
 * structural bytes.
 * @throws GraphError when checked structural arithmetic overflows.
 * @note These are distinct original allocations, not duplicate charges for
 * the context copies. Image/backend/plugin output payloads remain excluded.
 */
template <typename DirtyPlan>
std::uint64_t prepared_dirty_retained_memory_bytes(
    const PreparedDirtyPlan<DirtyPlan>& prepared) {
  RetainedMemoryEstimator estimate("prepared dirty request");
  if constexpr (std::is_same_v<DirtyPlan, HighPrecisionDirtyPlan>) {
    estimate.add_bytes(
        high_precision_dirty_plan_retained_memory_bytes(prepared.dirty_plan));
  } else {
    static_assert(std::is_same_v<DirtyPlan, RealTimeDirtyPlan>);
    estimate.add_bytes(
        real_time_dirty_plan_retained_memory_bytes(prepared.dirty_plan));
  }
  estimate.add_objects<ComputePlan>();
  estimate.add_bytes(
      compute_plan_dynamic_retained_memory_bytes(prepared.compute_plan));
  estimate.add_objects<DirtyTaskSelectionOverlay>();
  estimate.add_bytes(
      dirty_selection_dynamic_retained_memory_bytes(prepared.selection));
  estimate.add_objects<DirtyUpdateWorkSet>();
  estimate.add_objects<int>(static_cast<std::uint64_t>(
      prepared.work_set.dirty_source_task_ids.capacity()));
  estimate.add_objects<int>(static_cast<std::uint64_t>(
      prepared.work_set.downstream_task_ids.capacity()));
  estimate.add_objects<std::vector<int>>(2U);
  estimate.add_objects<int>(
      static_cast<std::uint64_t>(prepared.source_task_ids.capacity()));
  estimate.add_objects<int>(
      static_cast<std::uint64_t>(prepared.downstream_task_ids.capacity()));
  return estimate.bytes();
}

}  // namespace

const NodeOutput* StabilizedDirtyParameters::find_staged_output(
    int node_id) const noexcept {
  const auto found = staged_outputs_.find(node_id);
  return found == staged_outputs_.end() ? nullptr : &found->second.output;
}

const NodeOutput* StabilizedDirtyParameters::find_parameter_output(
    int node_id) const noexcept {
  if (!parameter_producer_node_ids_.count(node_id)) {
    return nullptr;
  }
  return find_staged_output(node_id);
}

bool StabilizedDirtyParameters::geometry_affected(int node_id) const noexcept {
  return geometry_affected_node_ids_.count(node_id) != 0;
}

/** @copydoc StabilizedDirtyParameters::retained_memory_bytes */
std::uint64_t StabilizedDirtyParameters::retained_memory_bytes() const {
  RetainedMemoryEstimator estimate("StabilizedDirtyParameters");
  estimate.add_objects<StabilizedDirtyParameters>();
  estimate.add_shared_control_block();
  estimate.add_objects<decltype(staged_outputs_)::value_type>(
      static_cast<std::uint64_t>(staged_outputs_.size()));
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(staged_outputs_.size()));
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(staged_outputs_.size()));
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(staged_outputs_.size()));
  for (const auto& [node_id, staged] : staged_outputs_) {
    (void)node_id;
    estimate.add_bytes(
        node_output_dynamic_retained_memory_bytes(staged.output));
  }

  const auto add_set = [&estimate](const std::unordered_set<int>& values) {
    estimate.add_objects<void*>(
        static_cast<std::uint64_t>(values.bucket_count()));
    estimate.add_objects<std::unordered_set<int>::value_type>(
        static_cast<std::uint64_t>(values.size()));
    estimate.add_objects<void*>(static_cast<std::uint64_t>(values.size()));
    estimate.add_objects<void*>(static_cast<std::uint64_t>(values.size()));
  };
  add_set(staged_node_ids_);
  add_set(staged_source_node_ids_);
  add_set(parameter_producer_node_ids_);
  add_set(rt_satisfied_parameter_node_ids_);
  add_set(rt_required_parameter_node_ids_);
  add_set(geometry_affected_node_ids_);
  return estimate.bytes();
}

/** @copydoc
 * StabilizedDirtyParameters::missing_staged_output_entry_retained_memory_bytes
 */
std::uint64_t
StabilizedDirtyParameters::missing_staged_output_entry_retained_memory_bytes(
    const std::vector<int>& anticipated_node_ids) const {
  RetainedMemoryEstimator estimate("StabilizedDirtyParameters pending outputs");
  const NodeOutput empty_output;
  std::unordered_set<int> unique_node_ids;
  unique_node_ids.reserve(anticipated_node_ids.size());
  for (int node_id : anticipated_node_ids) {
    if (!unique_node_ids.insert(node_id).second ||
        staged_outputs_.find(node_id) != staged_outputs_.end()) {
      continue;
    }
    estimate.add_objects<decltype(staged_outputs_)::value_type>();
    estimate.add_objects<void*>(3U);
    estimate.add_bytes(node_output_dynamic_retained_memory_bytes(empty_output));
  }
  return estimate.bytes();
}

/** @copydoc stabilize_connected_dirty_parameters */
std::shared_ptr<const StabilizedDirtyParameters>
stabilize_connected_dirty_parameters(
    GraphModel& graph, GraphTraversalService& traversal, int target_node_id,
    uint64_t request_generation, uint64_t topology_generation,
    SchedulerTaskRuntime* task_runtime, ExecutionService* execution_service,
    SchedulerHostContext* host, ComputeRun* run,
    const ComputeRunLease* run_lease) {
  observe_dirty_run_or_throw(run, run_lease);
  if (execution_service != nullptr &&
      (task_runtime != nullptr || host == nullptr || run == nullptr)) {
    throw std::invalid_argument(
        "Connected-parameter service preflight requires only a host and Run.");
  }
  if (request_generation == 0) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Connected-parameter stabilization requires a non-zero "
                     "request generation.");
  }
  const std::vector<int> execution_order =
      traversal.topo_postorder_from(graph, target_node_id);
  observe_dirty_run_or_throw(run, run_lease);
  std::unordered_set<int> target_cone(execution_order.begin(),
                                      execution_order.end());
  auto result = std::make_shared<StabilizedDirtyParameters>();
  result->request_generation_ = request_generation;
  result->topology_generation_ = topology_generation;
  std::unordered_set<int> parameter_consumers;
  for (int node_id : execution_order) {
    const Node& node = graph.node(node_id);
    for (const ParameterInput& input : node.parameter_inputs) {
      if (input.from_node_id < 0) {
        continue;
      }
      if (!graph.has_node(input.from_node_id)) {
        throw GraphError(GraphErrc::MissingDependency,
                         "Parameter producer " +
                             std::to_string(input.from_node_id) +
                             " is missing for node " + std::to_string(node_id));
      }
      result->parameter_producer_node_ids_.insert(input.from_node_id);
      parameter_consumers.insert(node_id);
    }
  }
  if (result->parameter_producer_node_ids_.empty()) {
    observe_dirty_run_or_throw(run, run_lease);
    return result;
  }

  advance_dirty_run_for_execution(
      run, run_lease, execution_service != nullptr || task_runtime != nullptr);
  const std::vector<Device> available_devices =
      execution_service ? execution_service->available_devices()
                        : (task_runtime ? task_runtime->available_devices()
                                        : std::vector<Device>{Device::CPU});
  const auto available_devices_owner =
      std::make_shared<const std::vector<Device>>(available_devices);

  std::vector<int> closure_stack(result->parameter_producer_node_ids_.begin(),
                                 result->parameter_producer_node_ids_.end());
  result->staged_node_ids_ = result->parameter_producer_node_ids_;
  while (!closure_stack.empty()) {
    const int node_id = closure_stack.back();
    closure_stack.pop_back();
    for (const GraphTopologyEdge& edge : graph.upstream_edges(node_id)) {
      if (edge.from_node_id < 0 || !graph.has_node(edge.from_node_id)) {
        continue;
      }
      if (result->staged_node_ids_.insert(edge.from_node_id).second) {
        closure_stack.push_back(edge.from_node_id);
      }
    }
  }
  for (int node_id : result->staged_node_ids_) {
    bool has_staged_parent = false;
    for (const GraphTopologyEdge& edge : graph.upstream_edges(node_id)) {
      if (result->staged_node_ids_.count(edge.from_node_id)) {
        has_staged_parent = true;
        break;
      }
    }
    if (!has_staged_parent) {
      result->staged_source_node_ids_.insert(node_id);
    }
  }

  result->geometry_affected_node_ids_ = parameter_consumers;
  std::queue<int> affected_queue;
  for (int node_id : parameter_consumers) {
    affected_queue.push(node_id);
  }
  while (!affected_queue.empty()) {
    const int node_id = affected_queue.front();
    affected_queue.pop();
    for (const GraphTopologyEdge& edge : graph.downstream_edges(node_id)) {
      if (edge.kind != GraphTopologyEdgeKind::ImageInput ||
          !target_cone.count(edge.to_node_id)) {
        continue;
      }
      if (result->geometry_affected_node_ids_.insert(edge.to_node_id).second) {
        affected_queue.push(edge.to_node_id);
      }
    }
  }

  uint64_t preflight_task_id = 0U;
  for (int node_id : execution_order) {
    if (!result->staged_node_ids_.count(node_id)) {
      continue;
    }
    auto execute_preflight_node = [&graph, result, available_devices_owner,
                                   node_id, run, run_lease]() {
      observe_dirty_run_or_throw(run, run_lease);
      Node node_for_exec = graph.node(node_id);
      const NodeInputResolver::OutputLookup lookup =
          [&](int upstream_id) -> const NodeOutput* {
        if (const NodeOutput* staged =
                result->find_staged_output(upstream_id)) {
          return staged;
        }
        if (result->staged_node_ids_.count(upstream_id)) {
          return nullptr;
        }
        const Node* upstream = graph.find_node(upstream_id);
        return upstream ? ComputeCachePolicy::reusable_output(*upstream)
                        : nullptr;
      };
      const ResolvedNodeInputs resolved = NodeInputResolver::resolve(
          node_for_exec, lookup, "Connected-parameter stabilization");
      std::optional<OpRegistry::OpVariant> operation;
      const auto device_implementation =
          OpRegistry::instance().select_best_implementation(
              node_for_exec.type, node_for_exec.subtype,
              *available_devices_owner, ComputeIntent::GlobalHighPrecision);
      if (device_implementation) {
        operation = device_implementation->func;
      } else {
        const auto implementations = OpRegistry::instance().get_implementations(
            node_for_exec.type, node_for_exec.subtype);
        if (implementations && implementations->tiled_hp) {
          operation.emplace(*implementations->tiled_hp);
        } else if (implementations && implementations->monolithic_hp) {
          operation.emplace(*implementations->monolithic_hp);
        }
      }
      if (!operation) {
        throw GraphError(GraphErrc::NoOperation,
                         "No HP operation for connected-parameter preflight " +
                             node_for_exec.type + ":" + node_for_exec.subtype);
      }
      TiledExecutionConfig tiled_config;
      tiled_config.on_tile = [run, run_lease](const PixelRect&) {
        observe_dirty_run_or_throw(run, run_lease);
      };
      NodeOutput output =
          NodeExecutor::execute(graph, node_for_exec, *operation,
                                resolved.image_inputs, tiled_config);
      observe_dirty_run_or_throw(run, run_lease);
      if (!has_image_payload(output) && output.data.empty()) {
        throw GraphError(
            GraphErrc::ComputeError,
            "Connected-parameter preflight produced no output for " +
                node_for_exec.type + ":" + node_for_exec.subtype);
      }
      std::optional<PixelRect> hp_roi;
      if (has_image_payload(output)) {
        hp_roi = PixelRect{0, 0, output.image_buffer.width,
                           output.image_buffer.height};
      }
      result->staged_outputs_.emplace(
          node_id,
          StabilizedDirtyNodeOutput{
              std::move(output), graph.node(node_id).hp_version + 1, hp_roi});
    };
    if (execution_service) {
      auto owned_preflight =
          std::make_shared<std::function<void()>>(execute_preflight_node);
      ComputeRunLease lease =
          run_lease != nullptr ? *run_lease : run->acquire_lease();
      const ComputeRunTaskIdentity identity = lease.task_identity(
          std::numeric_limits<uint64_t>::max() - preflight_task_id);
      auto service_callback = [owned_preflight, node_id](
                                  ComputeRunLease& callback_lease,
                                  const ComputeRunTaskIdentity&,
                                  SchedulerTaskRuntime& service_runtime) {
        try {
          if (callback_lease.observe_cancellation().has_value()) {
            service_runtime.dec_tasks_to_complete();
            return;
          }
          service_runtime.log_event(SchedulerTraceAction::Execute, node_id);
          (*owned_preflight)();
          if (callback_lease.observe_cancellation().has_value()) {
            service_runtime.dec_tasks_to_complete();
            return;
          }
          service_runtime.dec_tasks_to_complete();
        } catch (...) {
          try {
            service_runtime.log_event(SchedulerTraceAction::RethrowException,
                                      node_id);
          } catch (...) {
          }
          throw;
        }
      };
      const ReadyTaskResourceDemand task_demand =
          owned_callback_resource_demand(
              static_cast<std::uint64_t>(sizeof(service_callback)));
      RetainedMemoryEstimator shared_demand("connected-parameter preflight");
      shared_demand.add_bytes(lease.retained_memory_bytes());
      shared_demand.add_bytes(result->retained_memory_bytes());
      shared_demand.add_bytes(
          result->missing_staged_output_entry_retained_memory_bytes({node_id}));
      shared_demand.add_objects<std::vector<Device>>();
      shared_demand.add_objects<Device>(
          static_cast<std::uint64_t>(available_devices_owner->capacity()));
      shared_demand.add_shared_control_block();
      shared_demand.add_objects<std::function<void()>>();
      shared_demand.add_shared_control_block();
      shared_demand.add_bytes(owned_callable_retained_memory_bytes(
          static_cast<std::uint64_t>(sizeof(execute_preflight_node))));
      std::vector<ReadyTaskSubmission> submissions;
      submissions.emplace_back(std::move(lease), identity, node_id, true,
                               std::move(service_callback),
                               SchedulerTaskPriority::High, task_demand);
      execution_service->execute_cpu_run(
          *host, std::move(submissions), 1,
          CpuRunResourceDemand{shared_demand.bytes(), task_demand});
      observe_dirty_run_or_throw(run, run_lease);
    } else if (task_runtime) {
      PreflightTaskExecutor<decltype(execute_preflight_node)> executor(
          execute_preflight_node, *task_runtime, node_id);
      std::vector<TaskHandle> handles{TaskHandle{&executor, 0, node_id}};
      task_runtime->submit_initial_task_handles(std::move(handles), 1,
                                                SchedulerTaskPriority::High);
      task_runtime->wait_for_completion();
      observe_dirty_run_or_throw(run, run_lease);
    } else {
      execute_preflight_node();
      observe_dirty_run_or_throw(run, run_lease);
    }
    ++preflight_task_id;
  }

  observe_dirty_run_or_throw(run, run_lease);

  for (int producer_id : result->parameter_producer_node_ids_) {
    const NodeOutput* output = result->find_staged_output(producer_id);
    if (!output) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Connected parameter producer " +
                           std::to_string(producer_id) +
                           " was not stabilized.");
    }
    if (has_image_payload(*output)) {
      result->rt_required_parameter_node_ids_.insert(producer_id);
    } else {
      result->rt_satisfied_parameter_node_ids_.insert(producer_id);
    }
  }
  return result;
}

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
 * @param run Optional standalone or realtime-child HP Run that owns staging,
 * task leases, and lifecycle state.
 * @param execution_service Optional fixed process CPU service used for owned
 * ready submissions.
 * @param run_lease Optional borrowed lifecycle lease observed across planning,
 * provider, tile, downsample, and commit boundaries.
 * @return Mutable target HP output owned by graph.
 * @throws std::bad_alloc unchanged when planning, task, cache, staging,
 * telemetry, or output storage exhausts memory.
 * @throws GraphError for planning, dependency, operation, scheduler, commit, or
 * target validation failures, including checked shared-resource estimation
 * and accepted cancellation at a cooperative boundary.
 * @note Planning and commit hold graph_mutex_ while scheduler/service work runs
 * outside that lock. Both standalone and realtime-child HP staging are
 * Run-owned. Per-node synchronization is request-local for standalone HP work
 * and shared only with the matching RT sibling when supplied by
 * ComputeService. Each process-service phase charges the complete shared
 * synchronization owner. Concurrent HP/RT siblings therefore reserve the same
 * object conservatively in both Runs so either reservation can settle first
 * without leaving the surviving sibling's retained ownership unaccounted.
 * Cancellation observations bracket planning, node/tile work, sibling gating,
 * write-buffer commit, downsample, and return. A monolithic provider already
 * entered remains non-preemptible, while product publication remains protected
 * by the outer request-owned staging/commit contender.
 */
NodeOutput& HighPrecisionDirtyExecutor::execute(
    GraphModel& graph, RealtimeProxyGraph& proxy_graph, GraphRuntime* runtime,
    const DirtyUpdateRequest& request, ComputeRun* run,
    ExecutionService* execution_service, const ComputeRunLease* run_lease) {
  observe_dirty_run_or_throw(run, run_lease);
  std::unique_lock<std::mutex> graph_lock(graph.graph_mutex_);

  if (request.stabilized_parameters &&
      request.stabilized_parameters->topology_generation() !=
          graph.topology_generation()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Graph topology changed after dirty preflight.");
  }

  if (request.force_recache) {
    graph.clear_full_task_graph_cache();
  }
  proxy_graph.synchronize_with_graph(graph);

  std::unique_ptr<GraphModel> planning_graph_owner;
  GraphModel* planning_graph = &graph;
  const std::unordered_set<int>* stabilized_geometry_nodes = nullptr;
  const std::unordered_set<int>* externally_satisfied_nodes = nullptr;
  if (request.stabilized_parameters &&
      request.stabilized_parameters->has_connected_parameters()) {
    planning_graph_owner = make_stabilized_planning_graph(
        graph, *request.stabilized_parameters, true);
    planning_graph = planning_graph_owner.get();
    stabilized_geometry_nodes =
        &request.stabilized_parameters->geometry_affected_node_ids();
    externally_satisfied_nodes =
        &request.stabilized_parameters->staged_node_ids();
  }

  RoiPropagationService roi_propagation;
  DirtyRegionPlanner dirty_planner(
      traversal_, roi_propagation, stabilized_geometry_nodes, nullptr,
      request.stabilized_parameters
          ? std::optional<uint64_t>(
                request.stabilized_parameters->request_generation())
          : std::nullopt);
  const PixelRect planning_roi =
      hp_planning_roi_for_request(*planning_graph, request);
  HighPrecisionDirtyPlan dirty_plan = dirty_planner.plan_high_precision(
      *planning_graph, request.node_id, planning_roi);
  auto prepared = prepare_dirty_execution(
      *planning_graph, std::move(dirty_plan),
      ComputeRequest{ComputeIntent::GlobalHighPrecision, request.node_id, false,
                     planning_roi},
      planning_graph != &graph ? &graph : nullptr, externally_satisfied_nodes);
  HighPrecisionDirtyPlan& prepared_dirty_plan = prepared.dirty_plan;
  observe_dirty_run_or_throw(run, run_lease);
  planning_graph_owner.reset();
  graph_lock.unlock();

  observe_dirty_run_or_throw(run, run_lease);

  std::optional<HighPrecisionDirtyWriteBuffer> local_hp_write_buffer;
  HighPrecisionDirtyWriteBuffer* hp_write_buffer_ptr = nullptr;
  if (run) {
    hp_write_buffer_ptr =
        &run->emplace_dirty_hp_write_buffer(!request.force_recache);
  } else {
    hp_write_buffer_ptr =
        &local_hp_write_buffer.emplace(!request.force_recache);
  }
  HighPrecisionDirtyWriteBuffer& hp_write_buffer = *hp_write_buffer_ptr;
  if (request.stabilized_parameters) {
    for (const auto& [node_id, staged] :
         request.stabilized_parameters->staged_outputs()) {
      hp_write_buffer.import_precomputed_output(
          graph.node(node_id), staged.output, staged.hp_version, staged.hp_roi,
          request.stabilized_parameters->is_staged_source(node_id)
              ? std::optional<uint64_t>(
                    request.stabilized_parameters->request_generation())
              : std::nullopt);
    }
  }
  std::shared_ptr<DirtyNodeSynchronization> node_synchronization =
      request.node_synchronization;
  if (!node_synchronization) {
    node_synchronization =
        make_dirty_node_synchronization(prepared.compute_plan);
  }
  DirtyNodeExecutionContext node_context{
      graph,
      runtime,
      events_,
      prepared_dirty_plan.snapshot,
      prepared_dirty_plan.snapshot.graph_generation,
      *node_synchronization,
      request.stabilized_parameters.get(),
      run_lease};
  HighPrecisionDirtyNodeExecutor node_executor(node_context, hp_write_buffer);

  auto run_hp_task = [&](int task_id) {
    observe_dirty_run_or_throw(run, run_lease);
    run_planned_dirty_task(
        runtime, prepared_dirty_plan.entries, prepared.compute_plan, task_id,
        [&](int node_id, HpPlanEntry& entry, const PlannedTask& task) {
          Node& node = graph.mutable_node(node_id);
          HpPlanEntry task_entry = entry_for_task(entry, task);
          observe_dirty_run_or_throw(run, run_lease);
          node_executor.execute(node, task_entry);
          observe_dirty_run_or_throw(run, run_lease);
        });
    observe_dirty_run_or_throw(run, run_lease);
  };
  auto validate_hp_source_boundaries = [&]() {
    observe_dirty_run_or_throw(run, run_lease);
    std::lock_guard<std::mutex> lock(graph.graph_mutex_);
    validate_hp_source_boundaries_ready(graph, prepared_dirty_plan.snapshot,
                                        hp_write_buffer);
  };

  advance_dirty_run_for_execution(run, run_lease, runtime != nullptr);
  DirtySourceFirstRunRequest source_first_request;
  source_first_request.runtime = runtime;
  source_first_request.intent = ComputeIntent::GlobalHighPrecision;
  source_first_request.execution_service = execution_service;
  source_first_request.host = runtime;
  source_first_request.run = run;
  source_first_request.run_lease = run_lease;
  source_first_request.compute_plan = &prepared.compute_plan;
  source_first_request.selection = &prepared.selection;
  RetainedMemoryEstimator hp_shared_demand("HP dirty request");
  hp_shared_demand.add_bytes(prepared_dirty_retained_memory_bytes(prepared));
  hp_shared_demand.add_bytes(node_synchronization->retained_memory_bytes());
  if (request.stabilized_parameters) {
    hp_shared_demand.add_bytes(
        request.stabilized_parameters->retained_memory_bytes());
  }
  source_first_request.additional_shared_retained_memory_bytes =
      hp_shared_demand.bytes();
  source_first_request.phase_shared_retained_memory_bytes =
      [&graph, &hp_write_buffer, &compute_plan = prepared.compute_plan](
          const std::vector<int>& task_ids) {
        return hp_write_buffer.missing_entry_retained_memory_bytes(
            graph, planned_nodes_for_task_ids(compute_plan, task_ids));
      };
  source_first_request.source_task_ids = &prepared.source_task_ids;
  source_first_request.downstream_task_ids = &prepared.downstream_task_ids;
  source_first_request.dirty_generation =
      prepared_dirty_plan.snapshot.graph_generation;
  source_first_request.before_downstream = validate_hp_source_boundaries;
  run_dirty_source_first(source_first_request, run_hp_task);
  observe_dirty_run_or_throw(run, run_lease);
  if (request.sibling_commit_gate) {
    request.sibling_commit_gate->wait_for_rt_commit_or_throw();
  }
  observe_dirty_run_or_throw(run, run_lease);
  if (run) {
    if (!run->advance_to(ComputeRunPhase::CommitPending)) {
      observe_dirty_run_or_throw(run, run_lease);
    }
  }
  graph_lock.lock();
  observe_dirty_run_or_throw(run, run_lease);
  if (request.stabilized_parameters &&
      request.stabilized_parameters->topology_generation() !=
          graph.topology_generation()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Graph topology changed during HP dirty execution.");
  }
  hp_write_buffer.commit_to_graph(graph);
  observe_dirty_run_or_throw(run, run_lease);
  if (!request.suppress_graph_downsample) {
    DownsampleExecutor(graph, proxy_graph, runtime, events_, run_lease)
        .execute(hp_write_buffer.downsample_requests());
    observe_dirty_run_or_throw(run, run_lease);
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
 * @param run Optional RT child Run owning task leases and lifecycle state.
 * @param execution_service Optional fixed process CPU service used for owned
 * ready submissions.
 * @param run_lease Optional borrowed lifecycle lease observed across planning,
 * provider, tile, and proxy-commit boundaries.
 * @return Mutable target RT output owned by proxy_graph.
 * @throws std::bad_alloc unchanged when planning, task, proxy, staging,
 * telemetry, or output storage exhausts memory.
 * @throws GraphError for planning, dependency, operation, scheduler, commit, or
 * target validation failures, including checked shared-resource estimation
 * and accepted cancellation at a cooperative boundary.
 * @note Planning and commit hold graph_mutex_ while scheduler/service work runs
 * outside that lock. RT output never becomes formal reusable GraphModel cache.
 * Each process-service phase charges the complete per-node synchronization
 * owner; a shared HP/RT sibling object is therefore conservatively present in
 * both independent Run reservations. Cancellation observations bracket
 * planning, node/tile work, proxy write-buffer commit, and return. A
 * monolithic provider already entered remains non-preemptible, while product
 * publication remains protected by the outer request-owned staging/commit
 * contender.
 */
NodeOutput& RealTimeDirtyExecutor::execute(
    GraphModel& graph, RealtimeProxyGraph& proxy_graph, GraphRuntime* runtime,
    const DirtyUpdateRequest& request, ComputeRun* run,
    ExecutionService* execution_service, const ComputeRunLease* run_lease) {
  observe_dirty_run_or_throw(run, run_lease);
  std::unique_lock<std::mutex> graph_lock(graph.graph_mutex_);

  if (request.stabilized_parameters &&
      request.stabilized_parameters->topology_generation() !=
          graph.topology_generation()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Graph topology changed after dirty preflight.");
  }

  if (request.force_recache) {
    graph.clear_full_task_graph_cache();
  }
  proxy_graph.synchronize_with_graph(graph);

  std::unique_ptr<GraphModel> planning_graph_owner;
  GraphModel* planning_graph = &graph;
  const std::unordered_set<int>* stabilized_geometry_nodes = nullptr;
  const std::unordered_set<int>* forced_parameter_producers = nullptr;
  const std::unordered_set<int>* externally_satisfied_nodes = nullptr;
  if (request.stabilized_parameters &&
      request.stabilized_parameters->has_connected_parameters()) {
    planning_graph_owner = make_stabilized_planning_graph(
        graph, *request.stabilized_parameters, false);
    planning_graph = planning_graph_owner.get();
    stabilized_geometry_nodes =
        &request.stabilized_parameters->geometry_affected_node_ids();
    forced_parameter_producers =
        &request.stabilized_parameters->rt_required_parameter_node_ids();
    externally_satisfied_nodes =
        &request.stabilized_parameters->rt_satisfied_parameter_node_ids();
  }

  RoiPropagationService roi_propagation;
  DirtyRegionPlanner dirty_planner(
      traversal_, roi_propagation, stabilized_geometry_nodes,
      forced_parameter_producers,
      request.stabilized_parameters
          ? std::optional<uint64_t>(
                request.stabilized_parameters->request_generation())
          : std::nullopt);
  RealTimeDirtyPlan dirty_plan = dirty_planner.plan_real_time(
      *planning_graph, request.node_id, request.dirty_roi);
  auto prepared = prepare_dirty_execution(
      *planning_graph, std::move(dirty_plan),
      ComputeRequest{ComputeIntent::RealTimeUpdate, request.node_id, false,
                     request.dirty_roi},
      planning_graph != &graph ? &graph : nullptr, externally_satisfied_nodes);
  RealTimeDirtyPlan& prepared_dirty_plan = prepared.dirty_plan;
  observe_dirty_run_or_throw(run, run_lease);
  planning_graph_owner.reset();
  if (request.force_recache) {
    reset_plan_cache(proxy_graph, prepared_dirty_plan);
  }
  graph_lock.unlock();

  observe_dirty_run_or_throw(run, run_lease);

  RealtimeProxyWriteBuffer rt_write_buffer(proxy_graph, !request.force_recache);
  std::shared_ptr<DirtyNodeSynchronization> node_synchronization =
      request.node_synchronization;
  if (!node_synchronization) {
    node_synchronization =
        make_dirty_node_synchronization(prepared.compute_plan);
  }
  DirtyNodeExecutionContext node_context{
      graph,
      runtime,
      events_,
      prepared_dirty_plan.snapshot,
      prepared_dirty_plan.snapshot.graph_generation,
      *node_synchronization,
      request.stabilized_parameters.get(),
      run_lease};
  RealTimeDirtyNodeExecutor node_executor(node_context, proxy_graph,
                                          rt_write_buffer);
  auto run_rt_task = [&](int task_id) {
    observe_dirty_run_or_throw(run, run_lease);
    run_planned_dirty_task(
        runtime, prepared_dirty_plan.entries, prepared.compute_plan, task_id,
        [&](int node_id, RtPlanEntry& entry, const PlannedTask& task) {
          Node& node = graph.mutable_node(node_id);
          RtPlanEntry task_entry = entry_for_task(entry, task);
          observe_dirty_run_or_throw(run, run_lease);
          node_executor.execute(node, task_entry);
          observe_dirty_run_or_throw(run, run_lease);
        });
    observe_dirty_run_or_throw(run, run_lease);
  };
  auto validate_rt_source_boundaries = [&]() {
    observe_dirty_run_or_throw(run, run_lease);
    std::lock_guard<std::mutex> lock(graph.graph_mutex_);
    validate_rt_source_boundaries_ready(
        graph, proxy_graph, prepared_dirty_plan.snapshot, rt_write_buffer);
  };

  advance_dirty_run_for_execution(run, run_lease, runtime != nullptr);
  DirtySourceFirstRunRequest source_first_request;
  source_first_request.runtime = runtime;
  source_first_request.intent = ComputeIntent::RealTimeUpdate;
  source_first_request.execution_service = execution_service;
  source_first_request.host = runtime;
  source_first_request.run = run;
  source_first_request.run_lease = run_lease;
  source_first_request.compute_plan = &prepared.compute_plan;
  source_first_request.selection = &prepared.selection;
  RetainedMemoryEstimator rt_shared_demand("RT dirty request");
  rt_shared_demand.add_bytes(prepared_dirty_retained_memory_bytes(prepared));
  rt_shared_demand.add_bytes(node_synchronization->retained_memory_bytes());
  if (request.stabilized_parameters) {
    rt_shared_demand.add_bytes(
        request.stabilized_parameters->retained_memory_bytes());
  }
  source_first_request.additional_shared_retained_memory_bytes =
      rt_shared_demand.bytes();
  source_first_request.phase_shared_retained_memory_bytes =
      [&rt_write_buffer, &compute_plan = prepared.compute_plan](
          const std::vector<int>& task_ids) {
        RetainedMemoryEstimator estimate("RT dirty staging phase");
        estimate.add_bytes(rt_write_buffer.retained_memory_bytes());
        estimate.add_bytes(rt_write_buffer.missing_entry_retained_memory_bytes(
            planned_nodes_for_task_ids(compute_plan, task_ids)));
        return estimate.bytes();
      };
  source_first_request.source_task_ids = &prepared.source_task_ids;
  source_first_request.downstream_task_ids = &prepared.downstream_task_ids;
  source_first_request.dirty_generation =
      prepared_dirty_plan.snapshot.graph_generation;
  source_first_request.before_downstream = validate_rt_source_boundaries;
  run_dirty_source_first(source_first_request, run_rt_task);
  observe_dirty_run_or_throw(run, run_lease);
  graph_lock.lock();
  observe_dirty_run_or_throw(run, run_lease);
  if (request.stabilized_parameters &&
      request.stabilized_parameters->topology_generation() !=
          graph.topology_generation()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Graph topology changed during RT dirty execution.");
  }
  graph_lock.unlock();
  observe_dirty_run_or_throw(run, run_lease);
  if (run) {
    if (!run->advance_to(ComputeRunPhase::CommitPending)) {
      observe_dirty_run_or_throw(run, run_lease);
    }
  }
  observe_dirty_run_or_throw(run, run_lease);
  rt_write_buffer.commit_to_proxy_graph();
  observe_dirty_run_or_throw(run, run_lease);
  return require_target_output(proxy_graph, request.node_id);
}

}  // namespace ps::compute
