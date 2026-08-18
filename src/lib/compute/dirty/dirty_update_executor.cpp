#include "compute/dirty/dirty_update_executor.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "compute/compute_geometry.hpp"
#include "compute/compute_run.hpp"
#include "compute/dirty/dirty_execution_common.hpp"
#include "compute/dirty/dirty_node_executor.hpp"
#include "compute/dirty/dirty_region_planner.hpp"
#include "compute/dirty/dirty_sibling_commit_gate.hpp"
#include "compute/dirty/dirty_write_buffers.hpp"
#include "compute/dirty/downsample_executor.hpp"
#include "compute/request/compute_cache_policy.hpp"
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
#include "compute/execution/execution_service_test_probe.hpp"
#endif
#include "compute/dirty/node_executor.hpp"
#include "compute/dirty/node_input_resolver.hpp"
#include "compute/dirty/realtime_proxy_graph.hpp"
#include "compute/execution/resource_demand_estimator.hpp"
#if defined(PHOTOSPIDER_INTERNAL_DIRTY_UPDATE_TESTING)
#include "compute/dirty/dirty_update_executor_test_access.hpp"
#endif
#include "core/value_image_adapter.hpp"
#include "graph/graph_extent_resolver.hpp"
#include "graph/graph_traversal_service.hpp"
#include "graph/roi_propagation_service.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/graph_runtime.hpp"

namespace ps::compute {

#if defined(PHOTOSPIDER_INTERNAL_DIRTY_UPDATE_TESTING)
namespace testing {
namespace {

/**
 * @brief Current test thread's borrowed dirty post-plan observer.
 * @throws Nothing for thread-local initialization and pointer access.
 * @note The separate internal test product owns this storage; the installable
 * archive compiles neither the state nor its notification branch.
 */
thread_local const DirtyPostPlanTestHook* g_dirty_post_plan_test_hook = nullptr;

}  // namespace

/** @copydoc set_dirty_post_plan_test_hook */
void set_dirty_post_plan_test_hook(const DirtyPostPlanTestHook* hook) noexcept {
  g_dirty_post_plan_test_hook = hook;
}

/** @copydoc notify_dirty_node_cache_plan_test_hook */
void notify_dirty_node_cache_plan_test_hook(const ComputePlan& node_cache_plan,
                                            GraphModel& graph) {
  if (g_dirty_post_plan_test_hook != nullptr &&
      g_dirty_post_plan_test_hook->notify_node_cache_plan != nullptr) {
    g_dirty_post_plan_test_hook->notify_node_cache_plan(
        g_dirty_post_plan_test_hook->context, node_cache_plan, graph);
  }
}

/**
 * @brief Notifies the current test thread after dirty planning completes.
 * @return Nothing.
 * @throws Any exception selected by the installed test observer.
 * @note A null hook or callback is a no-op. The graph planning lock has already
 * been released, and active-operation revalidation has not started.
 */
void notify_dirty_post_plan_test_hook() {
  if (g_dirty_post_plan_test_hook != nullptr &&
      g_dirty_post_plan_test_hook->notify != nullptr) {
    g_dirty_post_plan_test_hook->notify(g_dirty_post_plan_test_hook->context);
  }
}

}  // namespace testing
#endif

namespace {

/**
 * @brief Rejects a dirty/preflight boundary after accepted Run cancellation.
 * @param run Optional request observer for inline/private callers.
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
 * @param run Child or standalone Run, or null for direct private callers.
 * @param run_lease Preferred borrowed lifecycle lease for product callers.
 * @param queued Whether work crosses an execution-service queue.
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
 * @throws std::bad_alloc when copying Region metadata cannot allocate.
 * @note Tile tasks execute one derived physical tile. The authoritative Region
 * remains plan-level because publication occurs only after every selected task
 * succeeds; monolithic/TensorSlice work keeps the same Region and ROI.
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
 * @throws std::bad_alloc when copying Region metadata cannot allocate.
 * @note RT task output_roi is already in RT execution coordinates; HP Region
 * and ROI stay plan-level for commit after all selected tasks succeed.
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
 * @brief Selects and freezes one dirty operation plus its execution device.
 *
 * @param node Graph node whose operation is resolved.
 * @param available_devices Route-aware devices exposed before admission.
 * @param intent HP or RT registry priority policy.
 * @param require_tiled Whether the materialized task shape requires a tiled
 * callable.
 * @param planned_route Optional planning-time identity, device, shape, and
 * complete metadata contract.
 * @return Frozen callable/device pair, or nullopt when no compatible operation
 * exists.
 * @throws std::bad_alloc when registry snapshot storage allocates.
 * @throws Any exception propagated by registry callback copying.
 * @note The unified registry selection freezes callable, metadata, identity,
 * and device under one lock. When planned_route is supplied, any intervening
 * registry or metadata mutation rejects the operation instead of silently
 * changing it.
 */
std::optional<DirtyResolvedOperation> select_dirty_operation(
    const Node& node, const std::vector<Device>& available_devices,
    ComputeIntent intent, bool require_tiled,
    const PlannedOperationRoute* planned_route = nullptr,
    const PlannedOutputAuthority* planned_output_authority = nullptr) {
  auto selected = OpRegistry::instance().select_implementation(
      node.type, node.subtype, available_devices, intent,
      [require_tiled](const OpImplementation& implementation) {
        return !require_tiled || implementation.is_tiled();
      });
  if (!selected) {
    return std::nullopt;
  }
  if (planned_route != nullptr &&
      !planned_operation_route_matches(*planned_route, *selected)) {
    return std::nullopt;
  }
  PlannedOutputAuthority output_authority =
      planned_output_authority != nullptr
          ? *planned_output_authority
          : make_planned_output_authority(
                make_planned_operation_route(*selected), PixelSize{});
  return DirtyResolvedOperation{
      std::move(selected->func),         selected->metadata.device_preference,
      selected->implementation_identity, std::move(selected->metadata),
      std::move(output_authority),       std::move(selected->dirty_propagator)};
}

/**
 * @brief Freezes one dirty-domain output extent on a trusted route authority.
 * @param authority Frozen route authority to refine.
 * @param extent Positive HP or RT domain-local output size, or fully unknown.
 * @return Nothing.
 * @throws GraphError with ComputeError for a partially positive extent.
 * @note Non-image routes retain no image extent. Provider-returned descriptors
 * never participate in this refinement.
 */
void freeze_dirty_output_extent(PlannedOutputAuthority* authority,
                                const PixelSize& extent) {
  if (authority == nullptr) {
    throw GraphError(GraphErrc::ComputeError,
                     "Dirty output extent requires an authority.");
  }
  if (!authority->image_output_name.has_value()) {
    authority->image_extent.reset();
    return;
  }
  if ((extent.width > 0) != (extent.height > 0)) {
    throw GraphError(GraphErrc::ComputeError,
                     "Dirty output plan has a partially positive extent.");
  }
  authority->image_extent = extent.width > 0 && extent.height > 0
                                ? std::optional<PixelSize>(extent)
                                : std::nullopt;
}

/**
 * @brief Refines every HP planned authority from the trusted dirty plan.
 * @param compute_plan Materialized route/output plan mutated before admission.
 * @param entries HP plan entries carrying domain-local output extents.
 * @return Nothing after every matching work item is sealed.
 * @throws GraphError for absent authority or invalid planned extent.
 * @note Only host-planned HP geometry participates; provider results are not
 * inspected and cannot widen or replace the authority.
 */
void freeze_hp_plan_output_extents(
    ComputePlan* compute_plan,
    const std::unordered_map<int, HpPlanEntry>& entries) {
  if (compute_plan == nullptr) {
    throw GraphError(GraphErrc::ComputeError,
                     "HP output refinement requires a compute plan.");
  }
  for (PlannedNodeWork& work : compute_plan->planned_work) {
    const auto entry = entries.find(work.node_id);
    if (entry == entries.end()) {
      continue;
    }
    if (!work.output_authority.has_value()) {
      throw GraphError(GraphErrc::ComputeError,
                       "HP planned work lacks output authority.");
    }
    freeze_dirty_output_extent(&*work.output_authority, entry->second.hp_size);
  }
}

/**
 * @brief Refines every RT planned authority from the trusted dirty plan.
 * @param compute_plan Materialized route/output plan mutated before admission.
 * @param entries RT plan entries carrying domain-local output extents.
 * @return Nothing after every matching work item is sealed.
 * @throws GraphError for absent authority or invalid planned extent.
 * @note The frozen RT extent describes post-normalization staging. Provider
 * image geometry is separately validated before host normalization.
 */
void freeze_rt_plan_output_extents(
    ComputePlan* compute_plan,
    const std::unordered_map<int, RtPlanEntry>& entries) {
  if (compute_plan == nullptr) {
    throw GraphError(GraphErrc::ComputeError,
                     "RT output refinement requires a compute plan.");
  }
  for (PlannedNodeWork& work : compute_plan->planned_work) {
    const auto entry = entries.find(work.node_id);
    if (entry == entries.end()) {
      continue;
    }
    if (!work.output_authority.has_value()) {
      throw GraphError(GraphErrc::ComputeError,
                       "RT planned work lacks output authority.");
    }
    freeze_dirty_output_extent(&*work.output_authority, entry->second.rt_size);
  }
}

/**
 * @brief Revalidates and freezes operations for active dirty task nodes.
 *
 * @param graph Graph containing the immutable node descriptions.
 * @param compute_plan Materialized task shape for the dirty domain.
 * @param selection Generation-local active task overlay.
 * @param available_devices Route-aware device inventory.
 * @param intent Registry ordering policy for the dirty domain.
 * @return Node-id map containing exactly the unique active task nodes.
 * @throws std::bad_alloc when temporary sets, maps, or callback copies
 * allocate.
 * @throws GraphError with `GraphErrc::NoOperation` when any active node lacks
 * its planning-time route or that route no longer resolves to the exact
 * callback identity, device, shape, and metadata.
 * @note Inactive and externally satisfied nodes are deliberately ignored. A
 * node with any active Tile task rejects monolithic candidates. This complete
 * active-node validation runs before constraint construction, retained-memory
 * estimation, source-first preparation, or physical admission.
 */
DirtyResolvedOperationMap resolve_dirty_operations(
    const GraphModel& graph, const ComputePlan& compute_plan,
    const DirtyTaskSelectionOverlay& selection,
    const std::vector<Device>& available_devices, ComputeIntent intent) {
  std::vector<int> active_node_ids;
  active_node_ids.reserve(selection.active_task_ids.size());
  std::unordered_set<int> active_nodes;
  std::unordered_set<int> tiled_nodes;
  for (int task_id : selection.active_task_ids) {
    if (task_id < 0 || static_cast<std::size_t>(task_id) >=
                           compute_plan.task_graph.tasks.size()) {
      throw GraphError(GraphErrc::ComputeError,
                       "Dirty task selection contains an invalid task id.");
    }
    const PlannedTask& task =
        compute_plan.task_graph.tasks.at(static_cast<std::size_t>(task_id));
    if (active_nodes.insert(task.node_id).second) {
      active_node_ids.push_back(task.node_id);
    }
    if (task.kind == PlannedTaskKind::Tile) {
      tiled_nodes.insert(task.node_id);
    }
  }

  DirtyResolvedOperationMap resolved;
  resolved.reserve(active_node_ids.size());
  for (int node_id : active_node_ids) {
    const auto planned_work = std::find_if(
        compute_plan.planned_work.begin(), compute_plan.planned_work.end(),
        [node_id](const PlannedNodeWork& work) {
          return work.node_id == node_id;
        });
    if (planned_work == compute_plan.planned_work.end() ||
        !planned_work->operation_route.has_value() ||
        !planned_work->output_authority.has_value()) {
      const Node& node = graph.node(node_id);
      throw GraphError(GraphErrc::NoOperation,
                       "Active dirty node has no planned operation route for " +
                           node.type + ":" + node.subtype);
    }
    auto operation = select_dirty_operation(
        graph.node(node_id), available_devices, intent,
        tiled_nodes.count(node_id) != 0U, &*planned_work->operation_route,
        &*planned_work->output_authority);
    if (!operation) {
      const Node& node = graph.node(node_id);
      throw GraphError(GraphErrc::NoOperation,
                       "Active dirty operation changed after planning for " +
                           node.type + ":" + node.subtype);
    }
    resolved.emplace(node_id, std::move(*operation));
  }
  return resolved;
}

/**
 * @brief Expands node-level device selection to dense dirty task ids.
 *
 * @param compute_plan Plan whose task ids index the returned vector.
 * @param resolved_operations Frozen node operation/device map.
 * @return Device vector aligned with `compute_plan.task_graph.tasks`.
 * @throws std::bad_alloc when vector storage allocates.
 * @note Inactive tasks retain the CPU default. Every active task has already
 * passed exact operation revalidation before this helper is called.
 */
std::vector<Device> dirty_task_devices(
    const ComputePlan& compute_plan,
    const DirtyResolvedOperationMap& resolved_operations) {
  std::vector<Device> devices(compute_plan.task_graph.tasks.size(),
                              Device::CPU);
  for (const PlannedTask& task : compute_plan.task_graph.tasks) {
    const auto operation_it = resolved_operations.find(task.node_id);
    if (operation_it != resolved_operations.end()) {
      devices.at(static_cast<std::size_t>(task.task_id)) =
          operation_it->second.device;
    }
  }
  return devices;
}

/**
 * @brief Counts executable selected tiled tasks for shared-binding joins.
 * @tparam IncludeTask Predicate matching the executor's pre-binding skip rule.
 * @param compute_plan Complete retained task inventory.
 * @param selection Generation-local active task overlay.
 * @param include_task Callable returning true exactly when one active Tile task
 * reaches Host binding allocation and later retires one completion.
 * @return Positive counts for exactly the selected nodes with executable Tile
 * work.
 * @throws GraphError when the overlay contains an invalid task id.
 * @throws std::overflow_error if a per-node count cannot be represented.
 * @throws std::bad_alloc when count-map storage cannot grow.
 * @note The frozen map is created before physical admission. The predicate is
 * derived only from immutable plan/entry geometry and performs no provider or
 * payload work. Executors never infer expected completion from callback start
 * order, and monolithic or pre-binding no-op tasks do not appear.
 */
template <typename IncludeTask>
std::unordered_map<int, std::size_t> selected_tiled_task_counts(
    const ComputePlan& compute_plan, const DirtyTaskSelectionOverlay& selection,
    IncludeTask include_task) {
  std::unordered_map<int, std::size_t> counts;
  counts.reserve(selection.active_task_ids.size());
  for (int task_id : selection.active_task_ids) {
    if (task_id < 0 || static_cast<std::size_t>(task_id) >=
                           compute_plan.task_graph.tasks.size()) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Dirty tiled-task count observed an invalid selected task id.");
    }
    const PlannedTask& task =
        compute_plan.task_graph.tasks.at(static_cast<std::size_t>(task_id));
    if (task.kind != PlannedTaskKind::Tile || !include_task(task)) {
      continue;
    }
    std::size_t& count = counts[task.node_id];
    if (count == std::numeric_limits<std::size_t>::max()) {
      throw std::overflow_error("Dirty tiled-task count exceeds size_t.");
    }
    ++count;
  }
  return counts;
}

/**
 * @brief Expands frozen dirty operation constraints to dense task ids.
 *
 * @param compute_plan Plan whose task ids index the returned vector.
 * @param resolved_operations Exact node operation snapshots.
 * @return Constraint vector aligned with `compute_plan.task_graph.tasks`.
 * @throws std::bad_alloc when vector or exclusive-key copies allocate.
 * @note Inactive tasks retain the all-default value. Every active task has
 * already passed exact operation revalidation before this helper is called.
 */
std::vector<OperationExecutionConstraints> dirty_task_constraints(
    const ComputePlan& compute_plan,
    const DirtyResolvedOperationMap& resolved_operations) {
  std::vector<OperationExecutionConstraints> constraints(
      compute_plan.task_graph.tasks.size());
  for (const PlannedTask& task : compute_plan.task_graph.tasks) {
    const auto operation_it = resolved_operations.find(task.node_id);
    if (operation_it == resolved_operations.end()) {
      continue;
    }
    const DirtyResolvedOperation& operation = operation_it->second;
    constraints.at(static_cast<std::size_t>(task.task_id)) =
        OperationExecutionConstraints{operation.implementation_identity,
                                      operation.metadata.reentrant,
                                      operation.metadata.maximum_parallelism,
                                      operation.metadata.exclusive_key};
  }
  return constraints;
}

/**
 * @brief Computes the uniform per-task operation demand for a dirty Run.
 *
 * @param operations Exact operation snapshots used by the Run.
 * @return Component-wise maximum retained/scratch demand and one work unit.
 * @throws Nothing; only bounded comparisons and value copies are performed.
 * @note Ready bytes remain adapter-owned and are added by the source-first
 * context. This maximum matches the service's uniform batch reservation model.
 */
ReadyTaskResourceDemand dirty_task_operation_resource_demand(
    const DirtyResolvedOperationMap& operations) noexcept {
  ReadyTaskResourceDemand demand =
      ReadyTaskSubmission::default_resource_demand();
  for (const auto& [node_id, operation] : operations) {
    static_cast<void>(node_id);
    demand.retained_memory_bytes = std::max(
        demand.retained_memory_bytes, operation.metadata.retained_memory_bytes);
    demand.scratch_bytes =
        std::max(demand.scratch_bytes, operation.metadata.scratch_bytes);
  }
  return demand;
}

/**
 * @brief Estimates structural storage retained by dirty operation snapshots.
 *
 * @param operations Immutable node operation/device map.
 * @return Checked visible map bucket, value, and linkage bytes.
 * @throws GraphError when checked retained-memory arithmetic overflows.
 * @note Every retained operation-snapshot key is charged by its actual copied
 * string capacity plus the null terminator. Opaque allocations inside provider
 * callable targets remain excluded, matching full-plan callback accounting.
 */
std::uint64_t dirty_operation_retained_memory_bytes(
    const DirtyResolvedOperationMap& operations) {
  RetainedMemoryEstimator estimate("dirty resolved operations");
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(operations.bucket_count()));
  estimate.add_objects<DirtyResolvedOperationMap::value_type>(
      static_cast<std::uint64_t>(operations.size()));
  estimate.add_objects<void*>(static_cast<std::uint64_t>(operations.size()) *
                              2U);
  for (const auto& [node_id, operation] : operations) {
    static_cast<void>(node_id);
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
    const std::uint64_t before_operation_key = estimate.bytes();
#endif
    estimate.add_string_payload(operation.metadata.exclusive_key);
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
    testing::notify_retained_operation_string_charge_for_testing(
        testing::RetainedOperationStringOwner::DirtyResolvedOperation,
        operation.metadata.exclusive_key, before_operation_key,
        estimate.bytes());
#endif
    estimate.add_objects<std::string>(static_cast<std::uint64_t>(
        operation.metadata.named_value_output_names.capacity()));
    for (const std::string& name :
         operation.metadata.named_value_output_names) {
      estimate.add_string_payload(name);
    }
    estimate.add_objects<std::string>(static_cast<std::uint64_t>(
        operation.metadata.parameter_output_names.capacity()));
    for (const std::string& name : operation.metadata.parameter_output_names) {
      estimate.add_string_payload(name);
    }
  }
  return estimate.bytes();
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
      runtime->log_event(GraphRuntime::ExecutionEvent::RETHROW_EXCEPTION,
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
 * transaction-wide object to both HP and RT siblings. It owns no execution
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
 * @brief Exposes one connected-parameter preflight callback as an execution
 * task handle.
 *
 * @tparam RunTask Request-local callback type.
 * @throws Nothing during construction; run_task() documents invocation and
 * runtime-publication exceptions.
 * @note The executor borrows its callback and execution runtime. It must remain
 * alive until the matching wait_for_completion() call returns or throws.
 */
template <typename RunTask>
class PreflightExecutionTaskExecutor final : public ExecutionTaskExecutor {
 public:
  /**
   * @brief Binds one preflight callback to its runtime completion contract.
   * @param run_task Callback that computes and stages one producer node.
   * @param task_runtime Runtime receiving trace and completion publication.
   * @param node_id Graph node id used in execution trace events.
   * @throws Nothing.
   */
  PreflightExecutionTaskExecutor(RunTask& run_task,
                                 ExecutionTaskRuntime& task_runtime,
                                 int node_id)
      : run_task_(run_task), task_runtime_(task_runtime), node_id_(node_id) {}

  /**
   * @brief Executes the sole preflight task and settles runtime accounting.
   * @param task_id Must be zero for this single-task executor.
   * @return Nothing.
   * @throws GraphError when task_id is invalid.
   * @throws The exact callback, trace, or completion-publication exception.
   * @note Rethrow tracing is best-effort so a hostile trace hook cannot
   * replace the authoritative task exception or prevent batch completion.
   */
  void run_task(int task_id) override {
    if (task_id != 0) {
      throw GraphError(GraphErrc::ComputeError,
                       "Connected-parameter preflight task id is invalid");
    }
    try {
      task_runtime_.log_event(ExecutionTraceAction::Execute, node_id_);
      run_task_();
      task_runtime_.dec_tasks_to_complete();
    } catch (...) {
      try {
        task_runtime_.log_event(ExecutionTraceAction::RethrowException,
                                node_id_);
      } catch (...) {
      }
      throw;
    }
  }

 private:
  /** @brief Borrowed request-local preflight callback. */
  RunTask& run_task_;
  /** @brief Borrowed execution runtime for the active initial batch. */
  ExecutionTaskRuntime& task_runtime_;
  /** @brief Node id reported in execution trace events. */
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
 * @return Requested zero-based storage ROI for normal updates, or the full
 * zero-based target HP storage extent for forced HP dirty updates.
 * @throws GraphError when a forced dirty update cannot derive a valid current
 * HP extent from the target node.
 * @throws std::bad_alloc unchanged when extent or diagnostic storage exhausts
 * memory.
 * @note Forced HP dirty updates do not seed existing HP output into the staging
 * buffer, so their dirty plan must cover the entire authoritative HP frame
 * before commit. ImageBounds origin remains logical metadata and is never
 * copied into this PixelRect compatibility boundary.
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
    if (target_output->has_image_value() &&
        target_output->image_value().image_facet().has_value()) {
      const ImageBounds& bounds = target_output->image_value().image_bounds();
      const std::size_t width = image_bounds_width(bounds);
      const std::size_t height = image_bounds_height(bounds);
      const std::size_t maximum =
          static_cast<std::size_t>(std::numeric_limits<int>::max());
      if (width > maximum || height > maximum) {
        throw GraphError(GraphErrc::InvalidParameter,
                         "Forced HP output extent exceeds PixelRect.");
      }
      return PixelRect{0, 0, static_cast<int>(width), static_cast<int>(height)};
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
 * @brief Reports whether an output carries a canonical image Value.
 * @param output Output produced by a parameter stabilization operation.
 * @return True exactly when the permanent named image Value is present.
 * @throws Nothing.
 * @note Compatibility staging is deliberately ignored and rejected at its
 * inbound adapter before this planning boundary.
 */
bool has_canonical_image_value(const NodeOutput& output) noexcept {
  return output.has_image_value();
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
 * installed. Staged outputs must already contain sealed named Values and
 * compatibility staging is rejected. The shadow has its own FullTaskGraph
 * cache and therefore cannot reuse a stale live-graph task expansion.
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
      node.hp_region.reset();
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
    if (staged.output.has_compatibility_image()) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Stabilized planning rejects compatibility image staging.");
    }
    for (const auto& [name, value] : staged.output.named_values) {
      if (name.empty() || !value.valid() ||
          !value.ready_fence().poll().ready()) {
        throw GraphError(
            GraphErrc::ComputeError,
            "Stabilized planning requires valid Ready named Values.");
      }
    }
    node_it->second.cached_output_high_precision = staged.output;
    node_it->second.hp_version = staged.hp_version;
    node_it->second.hp_region = staged.hp_region;
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

/**
 * @brief One frozen connected-preflight node and optional service root.
 *
 * @throws Nothing from movement after preparation allocations complete.
 * @note Exactly one of inline_task and service_prepared is active.
 */
struct PreparedConnectedDirtyNode final {
  /** @brief Exact topological node identity. */
  int node_id = -1;
  /** @brief Frozen inline/task-runtime provider callback. */
  std::function<void()> inline_task;
  /** @brief Complete process-service root reserved before installation. */
  std::optional<PreparedExecutionRun> service_prepared;
};

/**
 * @brief Complete unpublished connected-parameter preflight state.
 *
 * @throws std::bad_alloc when copied route, lease, result, or step ownership
 * grows.
 * @note The heap address remains stable while callbacks borrow its Run lease.
 * Prepared service roots retire before the shared result; that result retires
 * before the once-per-preflight umbrella reservation and borrowed Run lease.
 */
struct PreparedConnectedDirtyParametersState final {
  /**
   * @brief Captures stable execution owners for candidate preparation.
   * @param active_graph Graph read later by installed provider callbacks.
   * @param active_task_runtime Optional task-runtime route.
   * @param active_execution_service Optional process execution service.
   * @param active_direct_execution_service Optional authority for direct
   * provider gates and resource admission.
   * @param active_run Optional HP Run.
   * @param lifecycle_lease Optional strong HP lifecycle lease.
   * @throws Nothing except ComputeRunLease copy construction.
   */
  PreparedConnectedDirtyParametersState(
      GraphModel& active_graph, ExecutionTaskRuntime* active_task_runtime,
      ExecutionService* active_execution_service, ComputeRun* active_run,
      const ComputeRunLease* lifecycle_lease,
      ExecutionService* active_direct_execution_service)
      : graph(&active_graph),
        task_runtime(active_task_runtime),
        execution_service(active_execution_service),
        direct_execution_service(active_direct_execution_service),
        run(active_run),
        run_lease(lifecycle_lease != nullptr
                      ? std::optional<ComputeRunLease>(*lifecycle_lease)
                      : std::nullopt) {}

  /** @brief Borrowed request Graph stable through installed execution. */
  GraphModel* graph = nullptr;
  /** @brief Optional task runtime used only after installation. */
  ExecutionTaskRuntime* task_runtime = nullptr;
  /** @brief Optional process service owning pre-reserved roots. */
  ExecutionService* execution_service = nullptr;
  /** @brief Optional process authority wrapping direct provider callbacks. */
  ExecutionService* direct_execution_service = nullptr;
  /** @brief Optional request HP Run. */
  ComputeRun* run = nullptr;
  /** @brief Stable lease observed by every provider boundary. */
  std::optional<ComputeRunLease> run_lease;
  /** @brief Once-per-preflight Run/result/staging retained-memory root. */
  std::optional<PreparedExecutionSharedReservation> shared_reservation;
  /** @brief Mutable result built only by topologically serialized callbacks. */
  std::shared_ptr<StabilizedDirtyParameters> result;
  /** @brief Frozen topological callbacks and unique per-node service roots. */
  std::vector<PreparedConnectedDirtyNode> steps;
};

/** @copydoc PreparedConnectedDirtyParameters::PreparedConnectedDirtyParameters
 */
PreparedConnectedDirtyParameters::PreparedConnectedDirtyParameters() noexcept =
    default;  // NOLINT(whitespace/indent_namespace)

/** @copydoc PreparedConnectedDirtyParameters::PreparedConnectedDirtyParameters
 */
PreparedConnectedDirtyParameters::PreparedConnectedDirtyParameters(
    std::unique_ptr<PreparedConnectedDirtyParametersState> state) noexcept
    : state_(std::move(state)) {}

/** @copydoc PreparedConnectedDirtyParameters::PreparedConnectedDirtyParameters
 */
PreparedConnectedDirtyParameters::PreparedConnectedDirtyParameters(
    PreparedConnectedDirtyParameters&& other) noexcept = default;  // NOLINT

/** @copydoc PreparedConnectedDirtyParameters::operator= */
PreparedConnectedDirtyParameters& PreparedConnectedDirtyParameters::operator=(
    PreparedConnectedDirtyParameters&& other) noexcept {
  if (this != &other) {
    if (state_) {
      std::terminate();
    }
    state_ = std::move(other.state_);
  }
  return *this;
}

/** @copydoc PreparedConnectedDirtyParameters::~PreparedConnectedDirtyParameters
 */
PreparedConnectedDirtyParameters::~PreparedConnectedDirtyParameters() noexcept =
    default;

/** @copydoc prepare_connected_dirty_parameters */
PreparedConnectedDirtyParameters prepare_connected_dirty_parameters(
    GraphModel& graph, GraphTraversalService& traversal, int target_node_id,
    uint64_t request_generation, uint64_t topology_generation,
    ExecutionTaskRuntime* task_runtime, ExecutionService* execution_service,
    ExecutionHostContext* host, ComputeRun* run,
    const ComputeRunLease* run_lease, const std::string& execution_type,
    const std::vector<Device>* available_devices_override,
    ExecutionService* direct_execution_service) {
  observe_dirty_run_or_throw(run, run_lease);
  if (execution_service != nullptr && direct_execution_service != nullptr) {
    throw std::invalid_argument(
        "Connected-parameter execution cannot use worker and direct "
        "operation admission together.");
  }
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
  auto state = std::make_unique<PreparedConnectedDirtyParametersState>(
      graph, task_runtime, execution_service, run, run_lease,
      direct_execution_service);
  if ((execution_service != nullptr || direct_execution_service != nullptr) &&
      !state->run_lease.has_value() && run == nullptr) {
    throw std::invalid_argument(
        "Connected-parameter operation admission requires a Run lease.");
  }
  if ((execution_service != nullptr || direct_execution_service != nullptr) &&
      !state->run_lease.has_value()) {
    state->run_lease.emplace(run->acquire_lease());
  }
  auto result = std::make_shared<StabilizedDirtyParameters>();
  state->result = result;
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
    return PreparedConnectedDirtyParameters(std::move(state));
  }

  const std::vector<Device> available_devices =
      available_devices_override != nullptr
          ? *available_devices_override
          : (execution_service
                 ? execution_service->available_devices(execution_type)
                 : (task_runtime ? task_runtime->available_devices()
                                 : std::vector<Device>{Device::CPU}));

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

  std::vector<int> anticipated_node_ids;
  anticipated_node_ids.reserve(result->staged_node_ids_.size());
  for (int node_id : execution_order) {
    if (result->staged_node_ids_.count(node_id)) {
      anticipated_node_ids.push_back(node_id);
    }
  }
  state->steps.reserve(anticipated_node_ids.size());
  PreparedConnectedDirtyParametersState* state_ptr = state.get();
  uint64_t preflight_task_id = 0U;
  GraphExtentResolver preflight_extent_resolver;
  std::unordered_map<int, PixelSize> preflight_extent_cache;
  for (int node_id : execution_order) {
    if (!result->staged_node_ids_.count(node_id)) {
      continue;
    }
    const Node& selection_node = graph.node(node_id);
    auto selected_operation =
        select_dirty_operation(selection_node, available_devices,
                               ComputeIntent::GlobalHighPrecision, false);
    if (!selected_operation) {
      throw GraphError(GraphErrc::NoOperation,
                       "No HP operation for connected-parameter preflight " +
                           selection_node.type + ":" + selection_node.subtype);
    }
    freeze_dirty_output_extent(&selected_operation->output_authority,
                               preflight_extent_resolver.resolve_output_extent(
                                   graph, node_id, preflight_extent_cache));
    const Device selected_device = selected_operation->device;
    OperationExecutionConstraints operation_constraints{
        selected_operation->implementation_identity,
        selected_operation->metadata.reentrant,
        selected_operation->metadata.maximum_parallelism,
        selected_operation->metadata.exclusive_key};
    OperationExecutionConstraints submission_constraints(operation_constraints);
    RetainedMemoryEstimator retained_constraints(
        "connected-parameter operation constraints");
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
    const std::uint64_t before_operation_constraint =
        retained_constraints.bytes();
#endif
    retained_constraints.add_string_payload(
        operation_constraints.exclusive_key);
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
    testing::notify_retained_operation_string_charge_for_testing(
        testing::RetainedOperationStringOwner::
            ConnectedPreflightOperationConstraint,
        operation_constraints.exclusive_key, before_operation_constraint,
        retained_constraints.bytes());
    const std::uint64_t before_submission_constraint =
        retained_constraints.bytes();
#endif
    retained_constraints.add_string_payload(
        submission_constraints.exclusive_key);
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
    testing::notify_retained_operation_string_charge_for_testing(
        testing::RetainedOperationStringOwner::
            ConnectedPreflightSubmissionConstraint,
        submission_constraints.exclusive_key, before_submission_constraint,
        retained_constraints.bytes());
#endif
    const std::uint64_t retained_constraint_bytes =
        retained_constraints.bytes();
    const ReadyTaskResourceDemand operation_demand{
        selected_operation->metadata.retained_memory_bytes,
        selected_operation->metadata.scratch_bytes, 0U, 1U};
    PlannedOutputAuthority output_authority =
        selected_operation->output_authority;
    OpMetadata operation_metadata = selected_operation->metadata;
    std::optional<DirtyRoiPropFunc> dirty_propagator =
        selected_operation->dirty_propagator;
    const std::uint64_t implementation_identity =
        selected_operation->implementation_identity;
    OpRegistry::OpVariant operation = std::move(selected_operation->operation);
    auto execute_preflight_node = [state_ptr, result, node_id, operation,
                                   operation_constraints =
                                       std::move(operation_constraints),
                                   operation_demand,
                                   operation_metadata =
                                       std::move(operation_metadata),
                                   dirty_propagator =
                                       std::move(dirty_propagator),
                                   implementation_identity,
                                   output_authority =
                                       std::move(output_authority)]() {
      const ComputeRunLease* active_lease =
          state_ptr->run_lease.has_value() ? &*state_ptr->run_lease : nullptr;
      observe_dirty_run_or_throw(state_ptr->run, active_lease);
      Node node_for_exec = state_ptr->graph->node(node_id);
      const NodeInputResolver::OutputLookup lookup =
          [state_ptr, &result](int upstream_id) -> const NodeOutput* {
        if (const NodeOutput* staged =
                result->find_staged_output(upstream_id)) {
          return staged;
        }
        if (result->staged_node_ids_.count(upstream_id)) {
          return nullptr;
        }
        const Node* upstream = state_ptr->graph->find_node(upstream_id);
        return upstream ? ComputeCachePolicy::reusable_output(*upstream)
                        : nullptr;
      };
      const ResolvedNodeInputs resolved = NodeInputResolver::resolve(
          node_for_exec, lookup, "Connected-parameter stabilization");
      TiledExecutionConfig tiled_config;
      tiled_config.metadata = operation_metadata;
      tiled_config.dirty_propagator = dirty_propagator;
      tiled_config.implementation_identity = implementation_identity;
      tiled_config.on_tile = [state_ptr](const PixelRect&) {
        const ComputeRunLease* lease =
            state_ptr->run_lease.has_value() ? &*state_ptr->run_lease : nullptr;
        observe_dirty_run_or_throw(state_ptr->run, lease);
      };
      NodeOutput output;
      {
        OperationExecutionLease operation_lease;
        if (state_ptr->direct_execution_service != nullptr) {
          if (active_lease == nullptr) {
            throw std::logic_error(
                "Direct connected-parameter execution lost its Run lease.");
          }
          operation_lease =
              state_ptr->direct_execution_service->acquire_operation_execution(
                  *active_lease, operation_constraints, operation_demand);
        }
        output =
            NodeExecutor::execute(*state_ptr->graph, node_for_exec, operation,
                                  resolved.image_inputs, tiled_config);
      }
      observe_dirty_run_or_throw(state_ptr->run, active_lease);
      validate_planned_output(output, output_authority,
                              PlannedOutputReadiness::RequireReady);
      const RegionSet hp_region =
          value_image_adapter::full_node_output_region(output);
      result->staged_outputs_.emplace(
          node_id,
          StabilizedDirtyNodeOutput{
              std::move(output), state_ptr->graph->node(node_id).hp_version + 1,
              hp_region});
    };
    PreparedConnectedDirtyNode step;
    step.node_id = node_id;
    if (execution_service) {
      auto owned_preflight = std::make_shared<std::function<void()>>(
          std::move(execute_preflight_node));
      ComputeRunLease lease(*state->run_lease);
      const ComputeRunTaskIdentity identity = lease.task_identity(
          std::numeric_limits<uint64_t>::max() - preflight_task_id);
      auto service_callback = [owned_preflight, node_id](
                                  ComputeRunLease& callback_lease,
                                  const ComputeRunTaskIdentity&,
                                  ExecutionTaskRuntime& service_runtime) {
        try {
          if (callback_lease.observe_cancellation().has_value()) {
            service_runtime.dec_tasks_to_complete();
            return;
          }
          service_runtime.log_event(ExecutionTraceAction::Execute, node_id);
          (*owned_preflight)();
          if (callback_lease.observe_cancellation().has_value()) {
            service_runtime.dec_tasks_to_complete();
            return;
          }
          service_runtime.dec_tasks_to_complete();
        } catch (...) {
          try {
            service_runtime.log_event(ExecutionTraceAction::RethrowException,
                                      node_id);
          } catch (...) {
          }
          throw;
        }
      };
      ReadyTaskResourceDemand task_demand = owned_callback_resource_demand(
          static_cast<std::uint64_t>(sizeof(service_callback)));
      RetainedMemoryEstimator complete_task_retained(
          "connected-parameter operation task");
      complete_task_retained.add_bytes(task_demand.retained_memory_bytes);
      complete_task_retained.add_bytes(operation_demand.retained_memory_bytes);
      task_demand.retained_memory_bytes = complete_task_retained.bytes();
      task_demand.scratch_bytes = operation_demand.scratch_bytes;
      RetainedMemoryEstimator unique_shared_demand(
          "connected-parameter preflight node callback");
      unique_shared_demand.add_objects<std::function<void()>>();
      unique_shared_demand.add_shared_control_block();
      unique_shared_demand.add_bytes(owned_callable_retained_memory_bytes(
          static_cast<std::uint64_t>(sizeof(execute_preflight_node))));
      unique_shared_demand.add_bytes(retained_constraint_bytes);
      std::vector<ReadyTaskSubmission> submissions;
      submissions.emplace_back(
          std::move(lease), identity, node_id, true,
          std::move(service_callback), ExecutionTaskPriority::High, task_demand,
          selected_device, std::move(submission_constraints));
      step.service_prepared.emplace(execution_service->prepare_run(
          *host, execution_type, std::move(submissions), 1,
          CpuRunResourceDemand{unique_shared_demand.bytes(), task_demand}));
    } else {
      step.inline_task = std::move(execute_preflight_node);
    }
    state->steps.push_back(std::move(step));
    ++preflight_task_id;
  }

  if (execution_service != nullptr && !state->steps.empty()) {
    RetainedMemoryEstimator shared_demand(
        "connected-parameter preflight shared ownership");
    shared_demand.add_bytes(state->run_lease->retained_memory_bytes());
    shared_demand.add_bytes(result->retained_memory_bytes());
    shared_demand.add_bytes(
        result->missing_staged_output_entry_retained_memory_bytes(
            anticipated_node_ids));
    state->shared_reservation.emplace(
        execution_service->prepare_shared_reservation(*state->run_lease,
                                                      shared_demand.bytes()));
  }

  observe_dirty_run_or_throw(run, run_lease);
  return PreparedConnectedDirtyParameters(std::move(state));
}

/** @copydoc execute_prepared_connected_dirty_parameters */
std::shared_ptr<const StabilizedDirtyParameters>
execute_prepared_connected_dirty_parameters(
    PreparedConnectedDirtyParameters prepared) {
  if (!prepared.state_ || !prepared.state_->graph || !prepared.state_->result) {
    throw std::invalid_argument(
        "Connected-parameter execution requires active prepared state.");
  }
  std::unique_ptr<PreparedConnectedDirtyParametersState> state =
      std::move(prepared.state_);
  const ComputeRunLease* run_lease =
      state->run_lease.has_value() ? &*state->run_lease : nullptr;
  observe_dirty_run_or_throw(state->run, run_lease);
  if (state->execution_service != nullptr && !state->steps.empty() &&
      (!state->shared_reservation.has_value() ||
       !state->shared_reservation->active())) {
    throw std::logic_error(
        "Connected-parameter service preflight has no shared reservation.");
  }

  for (PreparedConnectedDirtyNode& step : state->steps) {
    if (state->execution_service != nullptr) {
      if (!step.service_prepared.has_value() ||
          !step.service_prepared->active()) {
        throw std::logic_error(
            "Connected-parameter service step is not prepared.");
      }
      state->execution_service->execute_prepared_run(
          std::move(*step.service_prepared));
      step.service_prepared.reset();
    } else if (state->task_runtime != nullptr) {
      if (!step.inline_task) {
        throw std::logic_error(
            "Connected-parameter task-runtime step has no callback.");
      }
      PreflightExecutionTaskExecutor<std::function<void()>> executor(
          step.inline_task, *state->task_runtime, step.node_id);
      std::vector<ExecutionTaskHandle> handles{
          ExecutionTaskHandle{&executor, 0, step.node_id}};
      state->task_runtime->submit_initial_task_handles(
          std::move(handles), 1, ExecutionTaskPriority::High);
      state->task_runtime->wait_for_completion();
    } else {
      if (!step.inline_task) {
        throw std::logic_error(
            "Connected-parameter inline step has no callback.");
      }
      step.inline_task();
    }
    observe_dirty_run_or_throw(state->run, run_lease);
  }

  const std::shared_ptr<StabilizedDirtyParameters> result = state->result;
  for (int producer_id : result->parameter_producer_node_ids_) {
    const NodeOutput* output = result->find_staged_output(producer_id);
    if (!output) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Connected parameter producer " +
                           std::to_string(producer_id) +
                           " was not stabilized.");
    }
    if (has_canonical_image_value(*output)) {
      result->rt_required_parameter_node_ids_.insert(producer_id);
    } else {
      result->rt_satisfied_parameter_node_ids_.insert(producer_id);
    }
  }
  return result;
}

/**
 * @brief Complete unpublished HP dirty domain state.
 *
 * @throws std::bad_alloc from copied request, plan, operation, staging, or
 * synchronization ownership.
 * @note The state has a stable heap address before node callbacks are built.
 * Physical phases are declared last and therefore roll back first.
 */
struct PreparedHighPrecisionDirtyRunState final {
  /**
   * @brief Captures all planned HP state and creates the write buffer.
   * @param active_graph Request-local Graph snapshot.
   * @param active_proxy Staged proxy graph.
   * @param active_runtime Optional route/trace owner.
   * @param dirty_request Copied request options and shared owners.
   * @param active_run Optional candidate Run.
   * @param lifecycle_lease Optional candidate lease.
   * @param event_service Borrowed event service.
   * @param prepared_plan Complete dirty/compute plan.
   * @param operations Pre-resolved operation map.
   * @param devices Task-aligned physical devices.
   * @param synchronization Complete per-node synchronization owner.
   * @param route_type Copied private execution route.
   * @throws std::bad_alloc or GraphError from request/staging construction.
   */
  PreparedHighPrecisionDirtyRunState(
      GraphModel& active_graph, RealtimeProxyGraph& active_proxy,
      GraphRuntime* active_runtime, const DirtyUpdateRequest& dirty_request,
      ComputeRun* active_run, const ComputeRunLease* lifecycle_lease,
      GraphEventService& event_service,
      PreparedDirtyPlan<HighPrecisionDirtyPlan> prepared_plan,
      DirtyResolvedOperationMap operations, std::vector<Device> devices,
      std::shared_ptr<DirtyNodeSynchronization> synchronization,
      std::string route_type)
      : graph(&active_graph),
        proxy_graph(&active_proxy),
        runtime(active_runtime),
        request(dirty_request),
        run(active_run),
        run_lease(lifecycle_lease != nullptr
                      ? std::optional<ComputeRunLease>(*lifecycle_lease)
                      : (active_run != nullptr
                             ? std::optional<ComputeRunLease>(
                                   active_run->acquire_lease())
                             : std::nullopt)),
        events(&event_service),
        prepared(std::move(prepared_plan)),
        resolved_operations(std::move(operations)),
        task_devices(std::move(devices)),
        node_synchronization(std::move(synchronization)),
        execution_type(std::move(route_type)) {
    if (run != nullptr) {
      hp_write_buffer =
          &run->emplace_dirty_hp_write_buffer(!request.force_recache);
    } else {
      local_hp_write_buffer = std::make_unique<HighPrecisionDirtyWriteBuffer>(
          !request.force_recache);
      hp_write_buffer = local_hp_write_buffer.get();
    }
  }

  /** @brief Request-local Graph snapshot. */
  GraphModel* graph = nullptr;
  /** @brief Staged RT proxy used for optional downsample. */
  RealtimeProxyGraph* proxy_graph = nullptr;
  /** @brief Optional route and trace owner. */
  GraphRuntime* runtime = nullptr;
  /** @brief Complete copied dirty request. */
  DirtyUpdateRequest request;
  /** @brief Optional candidate Run owning shared staging. */
  ComputeRun* run = nullptr;
  /** @brief Stable strong lease address used by callbacks. */
  std::optional<ComputeRunLease> run_lease;
  /** @brief Borrowed event sink. */
  GraphEventService* events = nullptr;
  /** @brief Complete HP dirty and compute plans. */
  PreparedDirtyPlan<HighPrecisionDirtyPlan> prepared;
  /** @brief Pre-resolved operation variants. */
  DirtyResolvedOperationMap resolved_operations;
  /** @brief Per-task immutable device choices. */
  std::vector<Device> task_devices;
  /** @brief Per-task exact-identity concurrency and exclusion constraints. */
  std::vector<OperationExecutionConstraints> task_constraints;
  /** @brief Uniform maximum operation retained/scratch demand for this Run. */
  ReadyTaskResourceDemand task_operation_resource_demand;
  /** @brief Frozen executable tile counts sealing each shared HP binding. */
  std::unordered_map<int, std::size_t> tiled_task_counts;
  /** @brief Complete request-local node synchronization. */
  std::shared_ptr<DirtyNodeSynchronization> node_synchronization;
  /** @brief Copied private route id. */
  std::string execution_type;
  /** @brief Local write buffer when no Run owns one. */
  std::unique_ptr<HighPrecisionDirtyWriteBuffer> local_hp_write_buffer;
  /** @brief Exact active write buffer, Run-owned or local. */
  HighPrecisionDirtyWriteBuffer* hp_write_buffer = nullptr;
  /** @brief Node executor borrowing only stable state fields above. */
  std::unique_ptr<HighPrecisionDirtyNodeExecutor> node_executor;
  /** @brief Both unpublished physical dirty phases. */
  PreparedDirtySourceFirstRun physical_phases;
};

/**
 * @brief Complete unpublished RT dirty domain state.
 *
 * @throws std::bad_alloc from copied request, plan, operation, proxy staging,
 * or synchronization ownership.
 * @note The state has a stable heap address before node callbacks are built.
 * Physical phases are destroyed first on rollback.
 */
struct PreparedRealTimeDirtyRunState final {
  /**
   * @brief Captures all planned RT state and creates the proxy write buffer.
   * @param active_graph Request-local Graph snapshot.
   * @param active_proxy Staged proxy graph.
   * @param active_runtime Optional route/trace owner.
   * @param dirty_request Copied request options and shared owners.
   * @param active_run Optional candidate Run.
   * @param lifecycle_lease Optional candidate lease.
   * @param event_service Borrowed event service.
   * @param prepared_plan Complete dirty/compute plan.
   * @param operations Pre-resolved operation map.
   * @param devices Task-aligned physical devices.
   * @param synchronization Complete per-node synchronization owner.
   * @param route_type Copied private execution route.
   * @throws std::bad_alloc or GraphError from request/staging construction.
   */
  PreparedRealTimeDirtyRunState(
      GraphModel& active_graph, RealtimeProxyGraph& active_proxy,
      GraphRuntime* active_runtime, const DirtyUpdateRequest& dirty_request,
      ComputeRun* active_run, const ComputeRunLease* lifecycle_lease,
      GraphEventService& event_service,
      PreparedDirtyPlan<RealTimeDirtyPlan> prepared_plan,
      DirtyResolvedOperationMap operations, std::vector<Device> devices,
      std::shared_ptr<DirtyNodeSynchronization> synchronization,
      std::string route_type)
      : graph(&active_graph),
        proxy_graph(&active_proxy),
        runtime(active_runtime),
        request(dirty_request),
        run(active_run),
        run_lease(lifecycle_lease != nullptr
                      ? std::optional<ComputeRunLease>(*lifecycle_lease)
                      : (active_run != nullptr
                             ? std::optional<ComputeRunLease>(
                                   active_run->acquire_lease())
                             : std::nullopt)),
        events(&event_service),
        prepared(std::move(prepared_plan)),
        resolved_operations(std::move(operations)),
        task_devices(std::move(devices)),
        node_synchronization(std::move(synchronization)),
        execution_type(std::move(route_type)),
        rt_write_buffer(std::make_unique<RealtimeProxyWriteBuffer>(
            active_proxy, !dirty_request.force_recache)) {}

  /** @brief Request-local Graph snapshot. */
  GraphModel* graph = nullptr;
  /** @brief Staged proxy graph and eventual commit owner. */
  RealtimeProxyGraph* proxy_graph = nullptr;
  /** @brief Optional route and trace owner. */
  GraphRuntime* runtime = nullptr;
  /** @brief Complete copied dirty request. */
  DirtyUpdateRequest request;
  /** @brief Optional candidate Run. */
  ComputeRun* run = nullptr;
  /** @brief Stable strong lease address used by callbacks. */
  std::optional<ComputeRunLease> run_lease;
  /** @brief Borrowed event sink. */
  GraphEventService* events = nullptr;
  /** @brief Complete RT dirty and compute plans. */
  PreparedDirtyPlan<RealTimeDirtyPlan> prepared;
  /** @brief Pre-resolved operation variants. */
  DirtyResolvedOperationMap resolved_operations;
  /** @brief Per-task immutable device choices. */
  std::vector<Device> task_devices;
  /** @brief Per-task exact-identity concurrency and exclusion constraints. */
  std::vector<OperationExecutionConstraints> task_constraints;
  /** @brief Uniform maximum operation retained/scratch demand for this Run. */
  ReadyTaskResourceDemand task_operation_resource_demand;
  /** @brief Frozen executable tile counts sealing each shared RT binding. */
  std::unordered_map<int, std::size_t> tiled_task_counts;
  /** @brief Complete request-local node synchronization. */
  std::shared_ptr<DirtyNodeSynchronization> node_synchronization;
  /** @brief Copied private route id. */
  std::string execution_type;
  /** @brief Complete request-local proxy staging buffer. */
  std::unique_ptr<RealtimeProxyWriteBuffer> rt_write_buffer;
  /** @brief Node executor borrowing only stable state fields above. */
  std::unique_ptr<RealTimeDirtyNodeExecutor> node_executor;
  /** @brief Both unpublished physical dirty phases. */
  PreparedDirtySourceFirstRun physical_phases;
};

/** @copydoc PreparedHighPrecisionDirtyRun::PreparedHighPrecisionDirtyRun */
PreparedHighPrecisionDirtyRun::PreparedHighPrecisionDirtyRun() noexcept =
    default;  // NOLINT

/** @copydoc PreparedHighPrecisionDirtyRun::PreparedHighPrecisionDirtyRun */
PreparedHighPrecisionDirtyRun::PreparedHighPrecisionDirtyRun(
    std::unique_ptr<PreparedHighPrecisionDirtyRunState> state) noexcept
    : state_(std::move(state)) {}

/** @copydoc PreparedHighPrecisionDirtyRun::PreparedHighPrecisionDirtyRun */
PreparedHighPrecisionDirtyRun::PreparedHighPrecisionDirtyRun(
    PreparedHighPrecisionDirtyRun&& other) noexcept = default;  // NOLINT

/** @copydoc PreparedHighPrecisionDirtyRun::operator= */
PreparedHighPrecisionDirtyRun& PreparedHighPrecisionDirtyRun::operator=(
    PreparedHighPrecisionDirtyRun&& other) noexcept {
  if (this != &other) {
    if (state_) {
      std::terminate();
    }
    state_ = std::move(other.state_);
  }
  return *this;
}

/** @copydoc PreparedHighPrecisionDirtyRun::~PreparedHighPrecisionDirtyRun */
PreparedHighPrecisionDirtyRun::~PreparedHighPrecisionDirtyRun() noexcept =
    default;  // NOLINT

/** @copydoc PreparedRealTimeDirtyRun::PreparedRealTimeDirtyRun */
PreparedRealTimeDirtyRun::PreparedRealTimeDirtyRun() noexcept = default;

/** @copydoc PreparedRealTimeDirtyRun::PreparedRealTimeDirtyRun */
PreparedRealTimeDirtyRun::PreparedRealTimeDirtyRun(
    std::unique_ptr<PreparedRealTimeDirtyRunState> state) noexcept
    : state_(std::move(state)) {}

/** @copydoc PreparedRealTimeDirtyRun::PreparedRealTimeDirtyRun */
PreparedRealTimeDirtyRun::PreparedRealTimeDirtyRun(
    PreparedRealTimeDirtyRun&& other) noexcept = default;  // NOLINT

/** @copydoc PreparedRealTimeDirtyRun::operator= */
PreparedRealTimeDirtyRun& PreparedRealTimeDirtyRun::operator=(
    PreparedRealTimeDirtyRun&& other) noexcept {
  if (this != &other) {
    if (state_) {
      std::terminate();
    }
    state_ = std::move(other.state_);
  }
  return *this;
}

/** @copydoc PreparedRealTimeDirtyRun::~PreparedRealTimeDirtyRun */
PreparedRealTimeDirtyRun::~PreparedRealTimeDirtyRun() noexcept = default;

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
    node.hp_region.reset();
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
 * @param runtime Optional route/trace owner; null executes work inline.
 * @param request Dirty target, ROI, cache, telemetry, and sibling-gate options.
 * @param run Optional standalone or realtime-child HP Run that owns staging,
 * task leases, and lifecycle state.
 * @param execution_service Optional fixed process CPU service used for owned
 * ready submissions.
 * @param run_lease Optional borrowed lifecycle lease observed across planning,
 * provider, tile, downsample, and commit boundaries.
 * @param direct_execution_service Optional process authority wrapping direct
 * provider callbacks; mutually exclusive with execution_service.
 * @return Mutable target HP output owned by graph.
 * @throws std::bad_alloc unchanged when planning, task, cache, staging,
 * telemetry, or output storage exhausts memory.
 * @throws GraphError for planning, dependency, operation, dispatch, commit, or
 * target validation failures, including checked shared-resource estimation
 * and accepted cancellation at a cooperative boundary.
 * @note Planning and commit hold graph_mutex_ while queued service work runs
 * outside that lock. Both standalone and realtime-child HP staging are
 * Run-owned. Per-node synchronization is request-local for standalone HP work
 * and shared only with the matching RT sibling when supplied by
 * ComputeService. Each process-service phase charges the complete shared
 * synchronization owner. Concurrent HP/RT siblings therefore reserve the same
 * object conservatively in both Runs so either reservation can settle first
 * without leaving the surviving sibling's retained ownership unaccounted.
 * Exact complete formal HP cache may cut request-local work; force-recache
 * disables that satisfaction without clearing visible output during fallible
 * preparation. Cancellation observations bracket planning, node/tile work,
 * sibling gating, write-buffer commit, downsample, and return. A monolithic
 * provider already entered remains non-preemptible, while product publication
 * remains protected by the outer request-owned staging/commit contender.
 */
NodeOutput& HighPrecisionDirtyExecutor::execute(
    GraphModel& graph, RealtimeProxyGraph& proxy_graph, GraphRuntime* runtime,
    const DirtyUpdateRequest& request, ComputeRun* run,
    ExecutionService* execution_service, const ComputeRunLease* run_lease,
    ExecutionService* direct_execution_service) {
  return execute_prepared(prepare(graph, proxy_graph, runtime, request, run,
                                  execution_service, run_lease,
                                  direct_execution_service));
}

/** @copydoc HighPrecisionDirtyExecutor::prepare */
PreparedHighPrecisionDirtyRun HighPrecisionDirtyExecutor::prepare(
    GraphModel& graph, RealtimeProxyGraph& proxy_graph, GraphRuntime* runtime,
    const DirtyUpdateRequest& request, ComputeRun* run,
    ExecutionService* execution_service, const ComputeRunLease* run_lease,
    ExecutionService* direct_execution_service) {
  observe_dirty_run_or_throw(run, run_lease);
  if (execution_service != nullptr && direct_execution_service != nullptr) {
    throw std::invalid_argument(
        "HP dirty execution cannot use worker and direct operation admission "
        "together.");
  }
  if (direct_execution_service != nullptr && run_lease == nullptr) {
    throw std::invalid_argument(
        "Direct HP dirty execution requires a Run lease.");
  }
  if (execution_service != nullptr && runtime == nullptr) {
    throw std::invalid_argument(
        "HP dirty service execution requires a Graph runtime host.");
  }
  const std::string execution_type =
      runtime != nullptr
          ? runtime->execution_route(ComputeIntent::GlobalHighPrecision)
                .execution_type
          : "cpu";
  const std::vector<Device> available_devices =
      execution_service != nullptr
          ? execution_service->available_devices(execution_type)
          : std::vector<Device>{Device::CPU};
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

  RoiPropagationService roi_propagation(available_devices,
                                        ComputeIntent::GlobalHighPrecision);
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
                     planning_roi, !request.force_recache},
      available_devices, externally_satisfied_nodes);
  observe_dirty_run_or_throw(run, run_lease);
  planning_graph_owner.reset();
  graph_lock.unlock();

#if defined(PHOTOSPIDER_INTERNAL_DIRTY_UPDATE_TESTING)
  testing::notify_dirty_post_plan_test_hook();
#endif
  observe_dirty_run_or_throw(run, run_lease);

  freeze_hp_plan_output_extents(&prepared.compute_plan,
                                prepared.dirty_plan.entries);
  DirtyResolvedOperationMap resolved_operations = resolve_dirty_operations(
      graph, prepared.compute_plan, prepared.selection, available_devices,
      ComputeIntent::GlobalHighPrecision);
  std::vector<Device> task_devices =
      dirty_task_devices(prepared.compute_plan, resolved_operations);
  std::vector<OperationExecutionConstraints> task_constraints =
      dirty_task_constraints(prepared.compute_plan, resolved_operations);
  const ReadyTaskResourceDemand task_operation_resource_demand =
      dirty_task_operation_resource_demand(resolved_operations);
  std::shared_ptr<DirtyNodeSynchronization> node_synchronization =
      request.node_synchronization;
  if (!node_synchronization) {
    node_synchronization =
        make_dirty_node_synchronization(prepared.compute_plan);
  }
  auto state = std::make_unique<PreparedHighPrecisionDirtyRunState>(
      graph, proxy_graph, runtime, request, run, run_lease, events_,
      std::move(prepared), std::move(resolved_operations),
      std::move(task_devices), std::move(node_synchronization), execution_type);
  state->task_constraints = std::move(task_constraints);
  state->task_operation_resource_demand = task_operation_resource_demand;
  state->tiled_task_counts = selected_tiled_task_counts(
      state->prepared.compute_plan, state->prepared.selection,
      [&](const PlannedTask& task) {
        const auto entry =
            state->prepared.dirty_plan.entries.find(task.node_id);
        return entry != state->prepared.dirty_plan.entries.end() &&
               !entry->second.region_hp.is_empty();
      });
  HighPrecisionDirtyPlan& prepared_dirty_plan = state->prepared.dirty_plan;
  HighPrecisionDirtyWriteBuffer& hp_write_buffer = *state->hp_write_buffer;
  if (request.stabilized_parameters) {
    for (const auto& [node_id, staged] :
         request.stabilized_parameters->staged_outputs()) {
      hp_write_buffer.import_precomputed_output(
          graph.node(node_id), staged.output, staged.hp_version,
          staged.hp_region,
          request.stabilized_parameters->is_staged_source(node_id)
              ? std::optional<uint64_t>(
                    request.stabilized_parameters->request_generation())
              : std::nullopt);
    }
  }
  DirtyNodeExecutionContext node_context{
      graph,
      runtime,
      events_,
      prepared_dirty_plan.snapshot,
      state->resolved_operations,
      prepared_dirty_plan.snapshot.graph_generation,
      *state->node_synchronization,
      request.stabilized_parameters.get(),
      state->run_lease.has_value() ? &*state->run_lease : nullptr,
      direct_execution_service,
      false,
      &state->tiled_task_counts};
  state->node_executor = std::make_unique<HighPrecisionDirtyNodeExecutor>(
      node_context, hp_write_buffer);

  PreparedHighPrecisionDirtyRunState* state_ptr = state.get();
  auto run_hp_task = [state_ptr](int task_id) {
    const ComputeRunLease* active_lease =
        state_ptr->run_lease.has_value() ? &*state_ptr->run_lease : nullptr;
    observe_dirty_run_or_throw(state_ptr->run, active_lease);
    run_planned_dirty_task(
        state_ptr->runtime, state_ptr->prepared.dirty_plan.entries,
        state_ptr->prepared.compute_plan, task_id,
        [state_ptr, active_lease](int node_id, HpPlanEntry& entry,
                                  const PlannedTask& task) {
          Node& node = state_ptr->graph->mutable_node(node_id);
          HpPlanEntry task_entry = entry_for_task(entry, task);
          observe_dirty_run_or_throw(state_ptr->run, active_lease);
          state_ptr->node_executor->execute(node, task_entry);
          observe_dirty_run_or_throw(state_ptr->run, active_lease);
        });
    observe_dirty_run_or_throw(state_ptr->run, active_lease);
  };
  auto validate_hp_source_boundaries = [state_ptr]() {
    const ComputeRunLease* active_lease =
        state_ptr->run_lease.has_value() ? &*state_ptr->run_lease : nullptr;
    observe_dirty_run_or_throw(state_ptr->run, active_lease);
    std::lock_guard<std::mutex> lock(state_ptr->graph->graph_mutex_);
    validate_hp_source_boundaries_ready(*state_ptr->graph,
                                        state_ptr->prepared.dirty_plan.snapshot,
                                        *state_ptr->hp_write_buffer);
  };

  DirtySourceFirstRunRequest source_first_request;
  source_first_request.runtime = runtime;
  source_first_request.intent = ComputeIntent::GlobalHighPrecision;
  source_first_request.execution_service = execution_service;
  source_first_request.execution_type = state->execution_type;
  source_first_request.host = runtime;
  source_first_request.run = run;
  source_first_request.run_lease =
      state->run_lease.has_value() ? &*state->run_lease : nullptr;
  source_first_request.compute_plan = &state->prepared.compute_plan;
  source_first_request.selection = &state->prepared.selection;
  source_first_request.task_devices = &state->task_devices;
  source_first_request.task_constraints = &state->task_constraints;
  source_first_request.task_operation_resource_demand =
      state->task_operation_resource_demand;
  source_first_request.task_output_context = state_ptr;
  source_first_request.snapshot_task_output = [](const void* context,
                                                 int node_id) {
    const auto* run_state =
        static_cast<const PreparedHighPrecisionDirtyRunState*>(context);
    return run_state->hp_write_buffer->copy_output(node_id);
  };
  RetainedMemoryEstimator hp_shared_demand("HP dirty request");
  hp_shared_demand.add_bytes(
      prepared_dirty_retained_memory_bytes(state->prepared));
  hp_shared_demand.add_bytes(
      state->node_synchronization->retained_memory_bytes());
  hp_shared_demand.add_bytes(
      dirty_operation_retained_memory_bytes(state->resolved_operations));
  hp_shared_demand.add_objects<Device>(
      static_cast<std::uint64_t>(state->task_devices.capacity()));
  hp_shared_demand.add_objects<OperationExecutionConstraints>(
      static_cast<std::uint64_t>(state->task_constraints.capacity()));
  for (const OperationExecutionConstraints& constraints :
       state->task_constraints) {
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
    const std::uint64_t before_constraint_key = hp_shared_demand.bytes();
#endif
    hp_shared_demand.add_string_payload(constraints.exclusive_key);
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
    testing::notify_retained_operation_string_charge_for_testing(
        testing::RetainedOperationStringOwner::
            DirtyHighPrecisionExecutionConstraint,
        constraints.exclusive_key, before_constraint_key,
        hp_shared_demand.bytes());
#endif
  }
  if (request.stabilized_parameters) {
    hp_shared_demand.add_bytes(
        request.stabilized_parameters->retained_memory_bytes());
  }
  source_first_request.additional_shared_retained_memory_bytes =
      hp_shared_demand.bytes();
  source_first_request.phase_shared_retained_memory_bytes =
      [state_ptr](const std::vector<int>& task_ids) {
        return state_ptr->hp_write_buffer->missing_entry_retained_memory_bytes(
            *state_ptr->graph, planned_nodes_for_task_ids(
                                   state_ptr->prepared.compute_plan, task_ids));
      };
  source_first_request.source_task_ids = &state->prepared.source_task_ids;
  source_first_request.downstream_task_ids =
      &state->prepared.downstream_task_ids;
  source_first_request.dirty_generation =
      prepared_dirty_plan.snapshot.graph_generation;
  source_first_request.before_downstream = validate_hp_source_boundaries;
  state->physical_phases = prepare_dirty_source_first(
      source_first_request, std::function<void(int)>(run_hp_task),
      static_cast<std::uint64_t>(sizeof(run_hp_task)));
  observe_dirty_run_or_throw(
      run, state->run_lease.has_value() ? &*state->run_lease : nullptr);
  return PreparedHighPrecisionDirtyRun(std::move(state));
}

/** @copydoc HighPrecisionDirtyExecutor::execute_prepared */
NodeOutput& HighPrecisionDirtyExecutor::execute_prepared(
    PreparedHighPrecisionDirtyRun prepared) {
  if (!prepared.state_) {
    throw std::invalid_argument(
        "HP dirty execution requires active preparation.");
  }
  std::unique_ptr<PreparedHighPrecisionDirtyRunState> state =
      std::move(prepared.state_);
  const ComputeRunLease* run_lease =
      state->run_lease.has_value() ? &*state->run_lease : nullptr;
  observe_dirty_run_or_throw(state->run, run_lease);
  advance_dirty_run_for_execution(state->run, run_lease,
                                  state->runtime != nullptr);
  {
    std::lock_guard<std::mutex> graph_lock(state->graph->graph_mutex_);
    observe_dirty_run_or_throw(state->run, run_lease);
    publish_prepared_dirty_inspection(*state->graph, state->prepared);
  }
  state->physical_phases.execute();
  observe_dirty_run_or_throw(state->run, run_lease);
  if (state->request.sibling_commit_gate) {
    state->request.sibling_commit_gate->wait_for_rt_commit_or_throw();
  }
  observe_dirty_run_or_throw(state->run, run_lease);
  if (state->run != nullptr) {
    if (!state->run->advance_to(ComputeRunPhase::CommitPending)) {
      observe_dirty_run_or_throw(state->run, run_lease);
    }
  }
  std::unique_lock<std::mutex> graph_lock(state->graph->graph_mutex_);
  observe_dirty_run_or_throw(state->run, run_lease);
  if (state->request.stabilized_parameters &&
      state->request.stabilized_parameters->topology_generation() !=
          state->graph->topology_generation()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Graph topology changed during HP dirty execution.");
  }
  state->hp_write_buffer->commit_to_graph(
      *state->graph, state->prepared.compute_plan.planned_work);
  observe_dirty_run_or_throw(state->run, run_lease);
  if (!state->request.suppress_graph_downsample) {
    DownsampleExecutor(*state->graph, *state->proxy_graph, state->runtime,
                       *state->events, run_lease)
        .execute(state->hp_write_buffer->downsample_requests());
    observe_dirty_run_or_throw(state->run, run_lease);
  }
  return require_target_output(*state->graph, state->request.node_id);
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
 * @param runtime Optional route/trace owner; null executes work inline.
 * @param request Dirty target, ROI, cache, and telemetry options.
 * @param run Optional RT child Run owning task leases and lifecycle state.
 * @param execution_service Optional fixed process CPU service used for owned
 * ready submissions.
 * @param run_lease Optional borrowed lifecycle lease observed across planning,
 * provider, tile, and proxy-commit boundaries.
 * @param direct_execution_service Optional process authority wrapping direct
 * provider callbacks; mutually exclusive with execution_service.
 * @return Mutable target RT output owned by proxy_graph.
 * @throws std::bad_alloc unchanged when planning, task, proxy, staging,
 * telemetry, or output storage exhausts memory.
 * @throws GraphError for planning, dependency, operation, dispatch, commit, or
 * target validation failures, including checked shared-resource estimation
 * and accepted cancellation at a cooperative boundary.
 * @note Planning and commit hold graph_mutex_ while queued service work runs
 * outside that lock. RT output never becomes formal reusable GraphModel cache.
 * Each process-service phase charges the complete per-node synchronization
 * owner; a shared HP/RT sibling object is therefore conservatively present in
 * both independent Run reservations. Formal HP cache never satisfies RT task
 * demand. Cancellation observations bracket planning, node/tile work, proxy
 * write-buffer commit, and return. A monolithic provider already entered
 * remains non-preemptible, while product publication remains protected by the
 * outer request-owned staging/commit contender.
 */
NodeOutput& RealTimeDirtyExecutor::execute(
    GraphModel& graph, RealtimeProxyGraph& proxy_graph, GraphRuntime* runtime,
    const DirtyUpdateRequest& request, ComputeRun* run,
    ExecutionService* execution_service, const ComputeRunLease* run_lease,
    ExecutionService* direct_execution_service) {
  return execute_prepared(prepare(graph, proxy_graph, runtime, request, run,
                                  execution_service, run_lease,
                                  direct_execution_service));
}

/** @copydoc RealTimeDirtyExecutor::prepare */
PreparedRealTimeDirtyRun RealTimeDirtyExecutor::prepare(
    GraphModel& graph, RealtimeProxyGraph& proxy_graph, GraphRuntime* runtime,
    const DirtyUpdateRequest& request, ComputeRun* run,
    ExecutionService* execution_service, const ComputeRunLease* run_lease,
    ExecutionService* direct_execution_service) {
  observe_dirty_run_or_throw(run, run_lease);
  if (execution_service != nullptr && direct_execution_service != nullptr) {
    throw std::invalid_argument(
        "RT dirty execution cannot use worker and direct operation admission "
        "together.");
  }
  if (direct_execution_service != nullptr && run_lease == nullptr) {
    throw std::invalid_argument(
        "Direct RT dirty execution requires a Run lease.");
  }
  if (execution_service != nullptr && runtime == nullptr) {
    throw std::invalid_argument(
        "RT dirty service execution requires a Graph runtime host.");
  }
  const std::string execution_type =
      runtime != nullptr
          ? runtime->execution_route(ComputeIntent::RealTimeUpdate)
                .execution_type
          : "cpu";
  const std::vector<Device> available_devices =
      execution_service != nullptr
          ? execution_service->available_devices(execution_type)
          : std::vector<Device>{Device::CPU};
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

  RoiPropagationService roi_propagation(available_devices,
                                        ComputeIntent::RealTimeUpdate);
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
                     request.dirty_roi, false},
      available_devices, externally_satisfied_nodes);
  observe_dirty_run_or_throw(run, run_lease);
  planning_graph_owner.reset();
  graph_lock.unlock();

#if defined(PHOTOSPIDER_INTERNAL_DIRTY_UPDATE_TESTING)
  testing::notify_dirty_post_plan_test_hook();
#endif
  observe_dirty_run_or_throw(run, run_lease);

  freeze_rt_plan_output_extents(&prepared.compute_plan,
                                prepared.dirty_plan.entries);
  DirtyResolvedOperationMap resolved_operations = resolve_dirty_operations(
      graph, prepared.compute_plan, prepared.selection, available_devices,
      ComputeIntent::RealTimeUpdate);
  std::vector<Device> task_devices =
      dirty_task_devices(prepared.compute_plan, resolved_operations);
  std::vector<OperationExecutionConstraints> task_constraints =
      dirty_task_constraints(prepared.compute_plan, resolved_operations);
  const ReadyTaskResourceDemand task_operation_resource_demand =
      dirty_task_operation_resource_demand(resolved_operations);
  std::shared_ptr<DirtyNodeSynchronization> node_synchronization =
      request.node_synchronization;
  if (!node_synchronization) {
    node_synchronization =
        make_dirty_node_synchronization(prepared.compute_plan);
  }
  auto state = std::make_unique<PreparedRealTimeDirtyRunState>(
      graph, proxy_graph, runtime, request, run, run_lease, events_,
      std::move(prepared), std::move(resolved_operations),
      std::move(task_devices), std::move(node_synchronization), execution_type);
  state->task_constraints = std::move(task_constraints);
  state->task_operation_resource_demand = task_operation_resource_demand;
  state->tiled_task_counts = selected_tiled_task_counts(
      state->prepared.compute_plan, state->prepared.selection,
      [&](const PlannedTask& task) {
        const auto entry =
            state->prepared.dirty_plan.entries.find(task.node_id);
        return entry != state->prepared.dirty_plan.entries.end() &&
               !is_rect_empty(entry_for_task(entry->second, task).roi_rt);
      });
  RealTimeDirtyPlan& prepared_dirty_plan = state->prepared.dirty_plan;
  DirtyNodeExecutionContext node_context{
      graph,
      runtime,
      events_,
      prepared_dirty_plan.snapshot,
      state->resolved_operations,
      prepared_dirty_plan.snapshot.graph_generation,
      *state->node_synchronization,
      request.stabilized_parameters.get(),
      state->run_lease.has_value() ? &*state->run_lease : nullptr,
      direct_execution_service,
      request.exact_factor_four_preview,
      &state->tiled_task_counts};
  state->node_executor = std::make_unique<RealTimeDirtyNodeExecutor>(
      node_context, proxy_graph, *state->rt_write_buffer);
  PreparedRealTimeDirtyRunState* state_ptr = state.get();
  auto run_rt_task = [state_ptr](int task_id) {
    const ComputeRunLease* active_lease =
        state_ptr->run_lease.has_value() ? &*state_ptr->run_lease : nullptr;
    observe_dirty_run_or_throw(state_ptr->run, active_lease);
    run_planned_dirty_task(
        state_ptr->runtime, state_ptr->prepared.dirty_plan.entries,
        state_ptr->prepared.compute_plan, task_id,
        [state_ptr, active_lease](int node_id, RtPlanEntry& entry,
                                  const PlannedTask& task) {
          Node& node = state_ptr->graph->mutable_node(node_id);
          RtPlanEntry task_entry = entry_for_task(entry, task);
          observe_dirty_run_or_throw(state_ptr->run, active_lease);
          state_ptr->node_executor->execute(node, task_entry);
          observe_dirty_run_or_throw(state_ptr->run, active_lease);
        });
    observe_dirty_run_or_throw(state_ptr->run, active_lease);
  };
  auto validate_rt_source_boundaries = [state_ptr]() {
    const ComputeRunLease* active_lease =
        state_ptr->run_lease.has_value() ? &*state_ptr->run_lease : nullptr;
    observe_dirty_run_or_throw(state_ptr->run, active_lease);
    std::lock_guard<std::mutex> lock(state_ptr->graph->graph_mutex_);
    validate_rt_source_boundaries_ready(
        *state_ptr->graph, *state_ptr->proxy_graph,
        state_ptr->prepared.dirty_plan.snapshot, *state_ptr->rt_write_buffer);
  };

  DirtySourceFirstRunRequest source_first_request;
  source_first_request.runtime = runtime;
  source_first_request.intent = ComputeIntent::RealTimeUpdate;
  source_first_request.execution_service = execution_service;
  source_first_request.execution_type = state->execution_type;
  source_first_request.host = runtime;
  source_first_request.run = run;
  source_first_request.run_lease =
      state->run_lease.has_value() ? &*state->run_lease : nullptr;
  source_first_request.compute_plan = &state->prepared.compute_plan;
  source_first_request.selection = &state->prepared.selection;
  source_first_request.task_devices = &state->task_devices;
  source_first_request.task_constraints = &state->task_constraints;
  source_first_request.task_operation_resource_demand =
      state->task_operation_resource_demand;
  source_first_request.task_output_context = state_ptr;
  source_first_request.snapshot_task_output = [](const void* context,
                                                 int node_id) {
    const auto* run_state =
        static_cast<const PreparedRealTimeDirtyRunState*>(context);
    return run_state->rt_write_buffer->copy_output(node_id);
  };
  RetainedMemoryEstimator rt_shared_demand("RT dirty request");
  rt_shared_demand.add_bytes(
      prepared_dirty_retained_memory_bytes(state->prepared));
  rt_shared_demand.add_bytes(
      state->node_synchronization->retained_memory_bytes());
  rt_shared_demand.add_bytes(
      dirty_operation_retained_memory_bytes(state->resolved_operations));
  rt_shared_demand.add_objects<Device>(
      static_cast<std::uint64_t>(state->task_devices.capacity()));
  rt_shared_demand.add_objects<OperationExecutionConstraints>(
      static_cast<std::uint64_t>(state->task_constraints.capacity()));
  for (const OperationExecutionConstraints& constraints :
       state->task_constraints) {
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
    const std::uint64_t before_constraint_key = rt_shared_demand.bytes();
#endif
    rt_shared_demand.add_string_payload(constraints.exclusive_key);
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
    testing::notify_retained_operation_string_charge_for_testing(
        testing::RetainedOperationStringOwner::DirtyRealTimeExecutionConstraint,
        constraints.exclusive_key, before_constraint_key,
        rt_shared_demand.bytes());
#endif
  }
  if (request.stabilized_parameters) {
    rt_shared_demand.add_bytes(
        request.stabilized_parameters->retained_memory_bytes());
  }
  source_first_request.additional_shared_retained_memory_bytes =
      rt_shared_demand.bytes();
  source_first_request.phase_shared_retained_memory_bytes =
      [state_ptr](const std::vector<int>& task_ids) {
        RetainedMemoryEstimator estimate("RT dirty staging phase");
        estimate.add_bytes(state_ptr->rt_write_buffer->retained_memory_bytes());
        estimate.add_bytes(
            state_ptr->rt_write_buffer->missing_entry_retained_memory_bytes(
                planned_nodes_for_task_ids(state_ptr->prepared.compute_plan,
                                           task_ids)));
        return estimate.bytes();
      };
  source_first_request.source_task_ids = &state->prepared.source_task_ids;
  source_first_request.downstream_task_ids =
      &state->prepared.downstream_task_ids;
  source_first_request.dirty_generation =
      prepared_dirty_plan.snapshot.graph_generation;
  source_first_request.before_downstream = validate_rt_source_boundaries;
  state->physical_phases = prepare_dirty_source_first(
      source_first_request, std::function<void(int)>(run_rt_task),
      static_cast<std::uint64_t>(sizeof(run_rt_task)));
  observe_dirty_run_or_throw(
      run, state->run_lease.has_value() ? &*state->run_lease : nullptr);
  return PreparedRealTimeDirtyRun(std::move(state));
}

/** @copydoc RealTimeDirtyExecutor::execute_prepared */
NodeOutput& RealTimeDirtyExecutor::execute_prepared(
    PreparedRealTimeDirtyRun prepared) {
  if (!prepared.state_) {
    throw std::invalid_argument(
        "RT dirty execution requires active preparation.");
  }
  std::unique_ptr<PreparedRealTimeDirtyRunState> state =
      std::move(prepared.state_);
  const ComputeRunLease* run_lease =
      state->run_lease.has_value() ? &*state->run_lease : nullptr;
  observe_dirty_run_or_throw(state->run, run_lease);
  if (state->request.force_recache) {
    reset_plan_cache(*state->proxy_graph, state->prepared.dirty_plan);
  }
  advance_dirty_run_for_execution(state->run, run_lease,
                                  state->runtime != nullptr);
  {
    std::lock_guard<std::mutex> graph_lock(state->graph->graph_mutex_);
    observe_dirty_run_or_throw(state->run, run_lease);
    publish_prepared_dirty_inspection(*state->graph, state->prepared);
  }
  state->physical_phases.execute();
  observe_dirty_run_or_throw(state->run, run_lease);
  {
    std::lock_guard<std::mutex> graph_lock(state->graph->graph_mutex_);
    observe_dirty_run_or_throw(state->run, run_lease);
    if (state->request.stabilized_parameters &&
        state->request.stabilized_parameters->topology_generation() !=
            state->graph->topology_generation()) {
      throw GraphError(GraphErrc::ComputeError,
                       "Graph topology changed during RT dirty execution.");
    }
  }
  observe_dirty_run_or_throw(state->run, run_lease);
  if (state->run != nullptr) {
    if (!state->run->advance_to(ComputeRunPhase::CommitPending)) {
      observe_dirty_run_or_throw(state->run, run_lease);
    }
  }
  observe_dirty_run_or_throw(state->run, run_lease);
  state->rt_write_buffer->commit_to_proxy_graph(
      state->prepared.compute_plan.planned_work);
  observe_dirty_run_or_throw(state->run, run_lease);
  return require_target_output(*state->proxy_graph, state->request.node_id);
}

}  // namespace ps::compute
