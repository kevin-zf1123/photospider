#include "compute/compute_task_submission.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "compute/compute_dispatch_plan_builder.hpp"
#include "compute/resource_demand_estimator.hpp"
#include "graph/graph_traversal_service.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Formats a graph node id with its name when still present.
 *
 * @param graph GraphModel used for node lookup.
 * @param node_id Node id being reported.
 * @return Human-readable scheduler error context.
 * @throws std::bad_alloc if string construction fails.
 * @note Missing nodes are reported by id only.
 */
std::string node_context(const GraphModel& graph, int node_id) {
  const Node* node = graph.find_node(node_id);
  if (!node) {
    return "node " + std::to_string(node_id);
  }
  return "node " + std::to_string(node_id) + " (" + node->name + ")";
}

/**
 * @brief Creates a scheduler-stage GraphError exception pointer.
 *
 * @param graph GraphModel used to enrich the node label.
 * @param node_id Node whose dependent release failed.
 * @param detail Original scheduling exception detail.
 * @return Exception pointer carrying GraphErrc::ComputeError.
 * @throws std::bad_alloc if the wrapped diagnostic cannot allocate.
 * @note This preserves scheduler-stage context without relabeling
 * std::bad_alloc.
 */
std::exception_ptr scheduling_failure(const GraphModel& graph, int node_id,
                                      const std::string& detail) {
  return std::make_exception_ptr(
      GraphError(GraphErrc::ComputeError, "Scheduling stage after " +
                                              node_context(graph, node_id) +
                                              " failed: " + detail));
}

/**
 * @brief Checks whether a candidate matches the planned task shape.
 *
 * @param impl Registered operation implementation candidate.
 * @param require_tiled Whether the task graph materializes tile work.
 * @return true when the implementation shape is compatible.
 * @throws Nothing.
 */
bool implementation_shape_compatible(const OpImplementation& impl,
                                     bool require_tiled) {
  return !require_tiled || impl.is_tiled();
}

/**
 * @brief Chooses a shape-compatible per-device implementation for HP compute.
 *
 * @param node Graph node whose operation is being resolved.
 * @param available_devices Devices exposed by the scheduler runtime.
 * @param require_tiled Whether the task graph requires TileOpFunc.
 * @return Selected operation variant, or nullopt.
 * @throws std::bad_alloc if registry candidate storage allocates
 * unsuccessfully.
 * @note Device priority and cost scoring remain registry policy.
 */
std::optional<OpRegistry::OpVariant> select_device_aware_hp_op(
    const Node& node, const std::vector<Device>& available_devices,
    bool require_tiled) {
  const OpRegistry& registry = OpRegistry::instance();
  const auto best = registry.select_best_implementation(
      node.type, node.subtype, available_devices,
      ComputeIntent::GlobalHighPrecision,
      [require_tiled](const OpImplementation& impl) {
        return implementation_shape_compatible(impl, require_tiled);
      });
  if (!best) {
    return std::nullopt;
  }
  return best->func;
}

/**
 * @brief Best-effort settles a batch whose bootstrap submission threw.
 *
 * @param task_runtime Runtime whose empty batch owns one completion unit.
 * @param failure Original bootstrap submission exception.
 * @return Nothing.
 * @throws Nothing; the caller rethrows failure unchanged.
 * @note wait_for_completion() is attempted only when set_exception() accepted
 * the failure, avoiding a wait on a runtime that rejected exception transport.
 */
void settle_rejected_bootstrap(SchedulerTaskRuntime& task_runtime,
                               const std::exception_ptr& failure) noexcept {
  bool exception_published = false;
  try {
    task_runtime.set_exception(failure);
    exception_published = true;
  } catch (...) {
  }
  if (!exception_published) {
    return;
  }
  try {
    task_runtime.wait_for_completion();
  } catch (...) {
  }
}

}  // namespace

/**
 * @brief Builds one Run-owned full HP scheduler submission plan.
 *
 * @param run_id Opaque namespace of the owning Run.
 * @param graph Graph used for planning and operation resolution.
 * @param traversal Traversal service used by plan construction.
 * @param node_id Requested target node.
 * @param available_devices Scheduler-exposed device labels.
 * @throws GraphError or standard exceptions from planning and allocation.
 */
TaskSubmissionPlan::TaskSubmissionPlan(ComputeRunId run_id, GraphModel& graph,
                                       GraphTraversalService& traversal,
                                       int node_id,
                                       std::vector<Device> available_devices)
    : run_id_(run_id),
      graph_(graph),
      compute_plan_(
          ComputeDispatchPlanBuilder(traversal).build_high_precision_plan(
              graph, node_id)),
      execution_order_(compute_plan_.planned_nodes),
      available_devices_(std::move(available_devices)),
      dependency_state_(execution_order_, compute_plan_.task_graph) {
  resolve_operations();
  temp_results_.resize(execution_order_.size());
  task_execution_states_ = std::vector<std::atomic<std::uint8_t>>(size());
  for (auto& state : task_execution_states_) {
    state.store(static_cast<std::uint8_t>(TaskExecutionState::Pending),
                std::memory_order_relaxed);
  }
}

/** @copydoc TaskSubmissionPlan::LegacyCompletionRecord::transfer_to_callback */
bool TaskSubmissionPlan::LegacyCompletionRecord::
    transfer_to_callback() noexcept {
  std::uint8_t expected =
      static_cast<std::uint8_t>(LegacyCompletionOwner::Plan);
  return owner_.compare_exchange_strong(
      expected, static_cast<std::uint8_t>(LegacyCompletionOwner::Callback),
      std::memory_order_acq_rel, std::memory_order_acquire);
}

/** @copydoc TaskSubmissionPlan::LegacyCompletionRecord::retire_plan_owned */
void TaskSubmissionPlan::LegacyCompletionRecord::retire_plan_owned() noexcept {
  std::uint8_t expected =
      static_cast<std::uint8_t>(LegacyCompletionOwner::Plan);
  if (owner_.compare_exchange_strong(
          expected, static_cast<std::uint8_t>(LegacyCompletionOwner::Retired),
          std::memory_order_acq_rel, std::memory_order_acquire)) {
    retire_runtime();
  }
}

/** @copydoc TaskSubmissionPlan::LegacyCompletionRecord::retire_callback_owned
 */
void TaskSubmissionPlan::LegacyCompletionRecord::
    retire_callback_owned() noexcept {
  std::uint8_t expected =
      static_cast<std::uint8_t>(LegacyCompletionOwner::Callback);
  if (owner_.compare_exchange_strong(
          expected, static_cast<std::uint8_t>(LegacyCompletionOwner::Retired),
          std::memory_order_acq_rel, std::memory_order_acquire)) {
    retire_runtime();
  }
}

/** @copydoc TaskSubmissionPlan::LegacyCompletionRecord::retire_runtime */
void TaskSubmissionPlan::LegacyCompletionRecord::retire_runtime() noexcept {
  try {
    if (runtime_ == nullptr) {
      std::terminate();
    }
    runtime_->dec_tasks_to_complete();
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc TaskSubmissionPlan::retained_memory_bytes */
std::uint64_t TaskSubmissionPlan::retained_memory_bytes() const {
  RetainedMemoryEstimator estimate("TaskSubmissionPlan");
  estimate.add_objects<TaskSubmissionPlan>();
  estimate.add_bytes(compute_plan_dynamic_retained_memory_bytes(compute_plan_));
  estimate.add_objects<int>(
      static_cast<std::uint64_t>(execution_order_.capacity()));
  estimate.add_objects<Device>(
      static_cast<std::uint64_t>(available_devices_.capacity()));
  estimate.add_bytes(dependency_state_.dynamic_retained_memory_bytes());
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(submitted_initial_task_ids_.bucket_count()));
  estimate.add_objects<decltype(submitted_initial_task_ids_)::value_type>(
      static_cast<std::uint64_t>(submitted_initial_task_ids_.size()));
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(submitted_initial_task_ids_.size()));
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(submitted_initial_task_ids_.size()));
  estimate.add_objects<std::atomic<std::uint8_t>>(
      static_cast<std::uint64_t>(task_execution_states_.capacity()));
  estimate.add_objects<std::shared_ptr<LegacyCompletionRecord>>(
      static_cast<std::uint64_t>(legacy_completion_records_.capacity()));
  estimate.add_objects<LegacyCompletionRecord>(
      static_cast<std::uint64_t>(legacy_completion_records_.size()));
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(legacy_completion_records_.size()));
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(legacy_completion_records_.size()));
  if (task_runner_) {
    estimate.add_bytes(task_runner_->retained_memory_bytes());
  }
  estimate.add_objects<std::optional<NodeOutput>>(
      static_cast<std::uint64_t>(temp_results_.capacity()));
  for (const std::optional<NodeOutput>& result : temp_results_) {
    if (result.has_value()) {
      estimate.add_bytes(node_output_dynamic_retained_memory_bytes(*result));
    }
  }
  estimate.add_objects<std::optional<OpRegistry::OpVariant>>(
      static_cast<std::uint64_t>(resolved_ops_.capacity()));
  return estimate.bytes();
}

/**
 * @brief Discovers one validated initial ready identity set.
 *
 * @return Initial composite identities in this Run namespace.
 * @throws GraphError when no initial identity exists for a nonempty plan.
 * @throws std::overflow_error when task count exceeds scheduler accounting.
 * @throws std::bad_alloc or std::out_of_range from ready discovery.
 */
std::vector<ComputeRunTaskIdentity>
TaskSubmissionPlan::initial_ready_identities() {
  std::vector<ComputeRunTaskIdentity> initial_identities;
  initial_identities.reserve(size());
  submitted_initial_task_ids_.clear();
  append_graph_ready_tasks(initial_identities);
  if (initial_identities.empty()) {
    append_zero_dependency_tasks(initial_identities);
  }
  if (!empty() && initial_identities.empty()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Full HP plan has no initial ready task.");
  }
  if (size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::overflow_error(
        "Full HP task count exceeds scheduler completion range.");
  }
  return initial_identities;
}

/**
 * @brief Builds one lease-routed ready submission.
 *
 * @param lease Matching Run lease.
 * @param identity Registered composite task identity.
 * @param is_initial_ready Whether initial discovery selected this task.
 * @return Move-owned service submission.
 * @throws std::out_of_range for an unregistered local identity.
 * @throws std::invalid_argument for a lease/identity mismatch.
 * @throws std::bad_alloc from submission ownership.
 */
ReadyTaskSubmission TaskSubmissionPlan::make_ready_submission(
    const ComputeRunLease& lease, const ComputeRunTaskIdentity& identity,
    bool is_initial_ready) {
  if (!contains_task_identity(identity)) {
    throw std::out_of_range(
        "Ready submission identity is not registered by this Run plan.");
  }
  const std::size_t task_index =
      static_cast<std::size_t>(identity.local_task_id().value());
  const int trace_node_id =
      compute_plan_.task_graph.tasks.at(task_index).node_id;
  return ReadyTaskSubmission(lease, identity, trace_node_id, is_initial_ready,
                             [](ComputeRunLease& ready_lease,
                                const ComputeRunTaskIdentity& ready_identity,
                                SchedulerTaskRuntime& task_runtime) {
                               ready_lease.execute_task(ready_identity,
                                                        task_runtime);
                             });
}

/**
 * @brief Installs the Run-owned node task runner once.
 *
 * @param context Borrowed services and Run-owned plan vectors.
 * @return Nothing.
 * @throws std::logic_error when already installed.
 * @throws std::bad_alloc from runner state allocation.
 */
void TaskSubmissionPlan::emplace_task_runner(NodeTaskRunnerContext context) {
  if (task_runner_) {
    throw std::logic_error(
        "TaskSubmissionPlan already owns a node task runner.");
  }
  task_runner_ = std::make_unique<NodeTaskRunner>(context);
}

/**
 * @brief Builds a registered composite task identity.
 *
 * @param task_id Dense task id.
 * @return Run/local identity.
 * @throws std::out_of_range when task_id is not registered.
 */
ComputeRunTaskIdentity TaskSubmissionPlan::task_identity(int task_id) const {
  if (task_id < 0 || task_id >= static_cast<int>(size()) ||
      compute_plan_.task_graph.tasks.at(task_id).task_id != task_id) {
    throw std::out_of_range(
        "TaskSubmissionPlan local task id is not registered.");
  }
  return ComputeRunTaskIdentity(
      run_id_, ComputeRunLocalTaskId(static_cast<std::uint64_t>(task_id)));
}

/**
 * @brief Checks Run namespace and dense local registration.
 *
 * @param identity Candidate composite identity.
 * @return true only when this plan registered the complete identity.
 * @throws Nothing.
 */
bool TaskSubmissionPlan::contains_task_identity(
    const ComputeRunTaskIdentity& identity) const noexcept {
  if (identity.run_id() != run_id_) {
    return false;
  }
  const std::uint64_t local_value = identity.local_task_id().value();
  if (local_value >= compute_plan_.task_graph.tasks.size()) {
    return false;
  }
  const PlannedTask& task =
      compute_plan_.task_graph.tasks[static_cast<std::size_t>(local_value)];
  return task.task_id >= 0 &&
         static_cast<std::uint64_t>(task.task_id) == local_value;
}

/**
 * @brief Submits all initial ready identities as scheduler-owned callbacks.
 *
 * @param lease Matching Run lease copied into callbacks.
 * @param task_runtime Active scheduler batch.
 * @return Nothing.
 * @throws GraphError when no initial identity exists for a nonempty plan.
 * @throws std::overflow_error when planned count exceeds scheduler integer
 * accounting.
 * @throws std::bad_alloc or scheduler exceptions from submission.
 * @note Cancellation closes the publication gate and retires every completion
 * unit not yet transferred to a materialized callback. The caller retains the
 * separate bootstrap completion unit.
 */
void TaskSubmissionPlan::submit_initial_ready_tasks(
    const ComputeRunLease& lease, SchedulerTaskRuntime& task_runtime) {
  const std::vector<ComputeRunTaskIdentity> initial_identities =
      initial_ready_identities();

  if (!initialize_legacy_completion_ledger(lease, task_runtime)) {
    return;
  }
  log_initial_assignments(task_runtime);
  for (const ComputeRunTaskIdentity& identity : initial_identities) {
    (void)publish_legacy_callback(lease, identity, task_runtime);
  }
}

/**
 * @brief Materializes the service-owned initial ready set.
 *
 * @param lease Matching Run lease copied into every submission.
 * @return Move-owned ready submissions.
 * @throws GraphError, std::overflow_error, std::out_of_range, or
 * std::bad_alloc from ready discovery and submission ownership.
 */
std::vector<ReadyTaskSubmission>
TaskSubmissionPlan::make_initial_ready_submissions(
    const ComputeRunLease& lease) {
  const std::vector<ComputeRunTaskIdentity> initial_identities =
      initial_ready_identities();
  std::vector<ReadyTaskSubmission> submissions;
  submissions.reserve(initial_identities.size());
  for (const ComputeRunTaskIdentity& identity : initial_identities) {
    submissions.push_back(make_ready_submission(lease, identity, true));
  }
  return submissions;
}

/**
 * @brief Executes one registered task and releases its dependents exactly once.
 *
 * @param identity Matching composite task identity.
 * @param lease Lease copied into dependent callbacks.
 * @param task_runtime Active scheduler runtime.
 * @return Nothing.
 * @throws std::invalid_argument when identity mismatches.
 * @throws std::logic_error when a duplicate callback enters.
 * @throws GraphError, std::bad_alloc, or operation/runtime exceptions.
 * @throws std::system_error when Run cancellation/outcome synchronization
 * fails.
 * @note Terminal state before entry skips provider work; terminal state after
 * provider return suppresses dependent release. The exact original exception
 * is rethrown after best-effort trace.
 */
void TaskSubmissionPlan::execute_task(const ComputeRunTaskIdentity& identity,
                                      const ComputeRunLease& lease,
                                      SchedulerTaskRuntime& task_runtime) {
  if (!contains_task_identity(identity)) {
    throw std::invalid_argument(
        "Task identity does not belong to this Run submission plan.");
  }
  (void)lease.observe_cancellation();
  if (lease.terminal_outcome().has_value()) {
    return;
  }
  const int task_id = static_cast<int>(identity.local_task_id().value());
  std::uint8_t expected =
      static_cast<std::uint8_t>(TaskExecutionState::Pending);
  if (!task_execution_states_.at(task_id).compare_exchange_strong(
          expected, static_cast<std::uint8_t>(TaskExecutionState::Executing),
          std::memory_order_acq_rel, std::memory_order_acquire)) {
    throw std::logic_error(
        "Run-local task identity entered execution more than once.");
  }
  if (!task_runner_) {
    task_execution_states_.at(task_id).store(
        static_cast<std::uint8_t>(TaskExecutionState::Failed),
        std::memory_order_release);
    throw GraphError(GraphErrc::ComputeError,
                     "TaskSubmissionPlan has no owned task runner.");
  }

  const PlannedTask& task = compute_plan_.task_graph.tasks.at(task_id);
  const SchedulerTraceAction execute_action =
      task.kind == PlannedTaskKind::Tile ? SchedulerTraceAction::ExecuteTile
                                         : SchedulerTraceAction::Execute;
  task_runtime.log_event(execute_action, task.node_id);
  try {
    task_runner_->run_task(task_id);
    (void)lease.observe_cancellation();
    if (!lease.terminal_outcome().has_value()) {
      release_dependents(task.task_id, task.node_id, lease, task_runtime);
    }
    task_execution_states_.at(task_id).store(
        static_cast<std::uint8_t>(TaskExecutionState::Completed),
        std::memory_order_release);
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    task_execution_states_.at(task_id).store(
        static_cast<std::uint8_t>(TaskExecutionState::Failed),
        std::memory_order_release);
    try {
      task_runtime.log_event(SchedulerTraceAction::RethrowException,
                             task.node_id);
    } catch (...) {
    }
    std::rethrow_exception(failure);
  }
}

/**
 * @brief Resolves operation variants once for all planned nodes.
 *
 * @return Nothing.
 * @throws std::bad_alloc from registry candidate or output allocation.
 */
void TaskSubmissionPlan::resolve_operations() {
  resolved_ops_.resize(execution_order_.size());
  for (std::size_t i = 0; i < execution_order_.size(); ++i) {
    const auto& node = graph_.node(execution_order_[i]);
    const bool has_tile_task = std::any_of(
        compute_plan_.task_graph.tasks.begin(),
        compute_plan_.task_graph.tasks.end(), [&](const PlannedTask& task) {
          return task.node_id == node.id && task.kind == PlannedTaskKind::Tile;
        });
    if (has_tile_task) {
      if (auto device_op =
              select_device_aware_hp_op(node, available_devices_, true)) {
        resolved_ops_[i] = std::move(*device_op);
        continue;
      }
      const auto impls =
          OpRegistry::instance().get_implementations(node.type, node.subtype);
      if (impls && impls->tiled_hp) {
        resolved_ops_[i] = OpRegistry::OpVariant{*impls->tiled_hp};
        continue;
      }
    }
    if (auto device_op =
            select_device_aware_hp_op(node, available_devices_, false)) {
      resolved_ops_[i] = std::move(*device_op);
      continue;
    }
    resolved_ops_[i] = OpRegistry::instance().resolve_for_intent(
        node.type, node.subtype, ComputeIntent::GlobalHighPrecision);
  }
}

/**
 * @brief Releases dependencies and submits matching lease-backed callbacks.
 *
 * @param current_task_id Completed local task id.
 * @param current_node_id Node used in scheduling diagnostics.
 * @param lease Matching lease copied into ready callbacks.
 * @param task_runtime Runtime receiving ready callbacks.
 * @return Nothing.
 * @throws std::bad_alloc unchanged.
 * @throws GraphError wrapping other dependency/submission failures.
 * @note Cancellation is checked before dependency counters mutate, and the
 * publication helpers recheck terminal closure before each ready submission.
 */
void TaskSubmissionPlan::release_dependents(
    int current_task_id, int current_node_id, const ComputeRunLease& lease,
    SchedulerTaskRuntime& task_runtime) {
  try {
    (void)lease.observe_cancellation();
    if (lease.terminal_outcome().has_value()) {
      return;
    }
    std::atomic_thread_fence(std::memory_order_release);
    const std::vector<int> ready_task_ids =
        dependency_state_.release_dependents(current_task_id);
    auto* ready_runtime =
        dynamic_cast<ReadyTaskSubmissionRuntime*>(&task_runtime);
    for (int dependent_task_id : ready_task_ids) {
      const ComputeRunTaskIdentity identity = task_identity(dependent_task_id);
      if (ready_runtime != nullptr) {
        (void)publish_service_submission(lease, identity, *ready_runtime);
      } else {
        (void)publish_legacy_callback(lease, identity, task_runtime);
      }
    }
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::out_of_range& error) {
    std::rethrow_exception(scheduling_failure(
        graph_, current_node_id, "out_of_range: " + std::string(error.what())));
  } catch (const std::exception& error) {
    std::rethrow_exception(
        scheduling_failure(graph_, current_node_id, error.what()));
  } catch (...) {
    std::rethrow_exception(
        scheduling_failure(graph_, current_node_id, "unknown exception"));
  }
}

/** @copydoc TaskSubmissionPlan::initialize_legacy_completion_ledger */
bool TaskSubmissionPlan::initialize_legacy_completion_ledger(
    const ComputeRunLease& lease, SchedulerTaskRuntime& task_runtime) {
  (void)lease.observe_cancellation();
  std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
  if (legacy_completion_initialized_) {
    throw std::logic_error(
        "TaskSubmissionPlan legacy completion ledger is already initialized.");
  }
  if (publication_closed_ || lease.terminal_outcome().has_value()) {
    publication_closed_ = true;
    return false;
  }

  std::vector<std::shared_ptr<LegacyCompletionRecord>> records;
  records.reserve(size());
  for (std::size_t index = 0U; index < size(); ++index) {
    records.push_back(std::make_shared<LegacyCompletionRecord>(task_runtime));
  }
  task_runtime.inc_tasks_to_complete(static_cast<int>(size()));
  legacy_completion_records_ = std::move(records);
  legacy_completion_initialized_ = true;

  if (lease.terminal_outcome().has_value()) {
    publication_closed_ = true;
    for (const auto& record : legacy_completion_records_) {
      record->retire_plan_owned();
    }
    return false;
  }
  return true;
}

/** @copydoc TaskSubmissionPlan::publish_legacy_callback */
bool TaskSubmissionPlan::publish_legacy_callback(
    const ComputeRunLease& lease, const ComputeRunTaskIdentity& identity,
    SchedulerTaskRuntime& task_runtime) {
  (void)lease.observe_cancellation();
  std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
  if (publication_closed_ || lease.terminal_outcome().has_value()) {
    return false;
  }
  if (!legacy_completion_initialized_) {
    throw std::logic_error(
        "Legacy callback publication requires an initialized completion "
        "ledger.");
  }
  const std::size_t task_index =
      static_cast<std::size_t>(identity.local_task_id().value());
  const std::shared_ptr<LegacyCompletionRecord>& record =
      legacy_completion_records_.at(task_index);
  auto token = std::make_shared<LegacyCompletionToken>(record);
  ComputeRunLease callback_lease = lease;
  SchedulerTaskRuntime::Task callback = [lease = std::move(callback_lease),
                                         identity, &task_runtime,
                                         token]() mutable {
    try {
      lease.execute_task(identity, task_runtime, true);
      token->retire();
    } catch (...) {
      token->retire();
      throw;
    }
  };
  if (!record->transfer_to_callback()) {
    return false;
  }
  token->arm();
  task_runtime.submit_ready_task_any_thread(std::move(callback));
  return true;
}

/** @copydoc TaskSubmissionPlan::publish_service_submission */
bool TaskSubmissionPlan::publish_service_submission(
    const ComputeRunLease& lease, const ComputeRunTaskIdentity& identity,
    ReadyTaskSubmissionRuntime& ready_runtime) {
  (void)lease.observe_cancellation();
  std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
  if (publication_closed_ || lease.terminal_outcome().has_value()) {
    return false;
  }
  ready_runtime.submit_ready_submission(
      make_ready_submission(lease, identity, false));
  return true;
}

/** @copydoc TaskSubmissionPlan::close_publication */
void TaskSubmissionPlan::close_publication() noexcept {
  try {
    std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
    if (publication_closed_) {
      return;
    }
    publication_closed_ = true;
    for (const auto& record : legacy_completion_records_) {
      record->retire_plan_owned();
    }
  } catch (...) {
    std::terminate();
  }
}

/**
 * @brief Appends one dependency-ready initial composite identity.
 *
 * @param task_id Dense task id candidate.
 * @param identities Output ready list.
 * @return Nothing.
 * @throws std::out_of_range for inconsistent task metadata.
 * @throws std::bad_alloc from deduplication or output growth.
 */
void TaskSubmissionPlan::append_initial_task_identity(
    int task_id, std::vector<ComputeRunTaskIdentity>& identities) {
  if (!dependency_state_.ready_for_initial_submit(task_id)) {
    return;
  }
  if (submitted_initial_task_ids_.insert(task_id).second) {
    identities.push_back(task_identity(task_id));
  }
}

/**
 * @brief Appends graph-declared initial ready identities.
 *
 * @param identities Output ready list.
 * @return Nothing.
 * @throws std::bad_alloc or std::out_of_range from ready discovery.
 */
void TaskSubmissionPlan::append_graph_ready_tasks(
    std::vector<ComputeRunTaskIdentity>& identities) {
  TaskGraphReadyChecker ready_checker;
  const std::vector<int> initial_ready_task_ids =
      ready_checker.initial_ready_task_ids(compute_plan_.task_graph);
  for (int task_id : initial_ready_task_ids) {
    if (task_id < 0 || task_id >= static_cast<int>(size())) {
      continue;
    }
    append_initial_task_identity(task_id, identities);
  }
}

/**
 * @brief Appends all zero-dependency identities as fallback.
 *
 * @param identities Output ready list.
 * @return Nothing.
 * @throws std::bad_alloc or std::out_of_range from identity construction.
 */
void TaskSubmissionPlan::append_zero_dependency_tasks(
    std::vector<ComputeRunTaskIdentity>& identities) {
  for (const auto& task : compute_plan_.task_graph.tasks) {
    append_initial_task_identity(task.task_id, identities);
  }
}

/**
 * @brief Emits AssignInitial traces for selected initial task ids.
 *
 * @param task_runtime Runtime receiving trace events.
 * @return Nothing.
 * @throws Exceptions from task_runtime.log_event().
 */
void TaskSubmissionPlan::log_initial_assignments(
    SchedulerTaskRuntime& task_runtime) const {
  for (int task_id : submitted_initial_task_ids_) {
    if (task_id < 0 || task_id >= static_cast<int>(size())) {
      continue;
    }
    const int node_id = compute_plan_.task_graph.tasks[task_id].node_id;
    if (std::find(execution_order_.begin(), execution_order_.end(), node_id) !=
        execution_order_.end()) {
      task_runtime.log_event(SchedulerTraceAction::AssignInitial, node_id);
    }
  }
}

/**
 * @brief Establishes one scheduler epoch and submits a leased bootstrap.
 *
 * @param graph Graph used to validate empty-plan target output.
 * @param task_runtime Runtime receiving the empty epoch batch and callbacks.
 * @param node_id Target node id.
 * @param plan Run-owned plan.
 * @param dispatcher_lease Lease copied into bootstrap callback.
 * @return Nothing.
 * @throws GraphError for an invalid empty plan.
 * @throws Scheduler or task exceptions unchanged.
 * @note The only TaskHandle batch is empty; full HP work uses owned callbacks.
 */
void dispatch_planned_tasks(GraphModel& graph,
                            SchedulerTaskRuntime& task_runtime, int node_id,
                            TaskSubmissionPlan& plan,
                            const ComputeRunLease& dispatcher_lease) {
  if (plan.empty() && graph.has_node(node_id)) {
    if (!graph.node(node_id).cached_output_high_precision.has_value()) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Planned dispatch produced no scheduler tasks for node " +
              std::to_string(node_id) +
              " and the target has no reusable high-precision output.");
    }
    return;
  }

  task_runtime.submit_initial_task_handles({}, 0);
  task_runtime.inc_tasks_to_complete(1);
  try {
    ComputeRunLease bootstrap_lease = dispatcher_lease;
    task_runtime.submit_ready_task_any_thread(
        [lease = std::move(bootstrap_lease), &task_runtime]() mutable {
          lease.execute_bootstrap(task_runtime);
        });
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    settle_rejected_bootstrap(task_runtime, failure);
    std::rethrow_exception(failure);
  }
  task_runtime.wait_for_completion();
}

/**
 * @brief Establishes one service-owned CPU batch from ready submissions.
 *
 * @param graph Graph used only for empty-plan target validation.
 * @param execution_service Injected process CPU service.
 * @param host Active Graph observation context.
 * @param node_id Requested target node.
 * @param plan Run-owned dispatcher submission plan.
 * @param dispatcher_lease Matching Run lease.
 * @return Nothing after service settlement.
 * @throws GraphError for an invalid empty plan.
 * @throws Service setup or exact task exceptions unchanged.
 */
void dispatch_planned_tasks(GraphModel& graph,
                            ExecutionService& execution_service,
                            SchedulerHostContext& host, int node_id,
                            TaskSubmissionPlan& plan,
                            const ComputeRunLease& dispatcher_lease) {
  if (plan.empty() && graph.has_node(node_id)) {
    if (!graph.node(node_id).cached_output_high_precision.has_value()) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Planned dispatch produced no scheduler tasks for node " +
              std::to_string(node_id) +
              " and the target has no reusable high-precision output.");
    }
    return;
  }

  std::vector<ReadyTaskSubmission> initial_submissions =
      plan.make_initial_ready_submissions(dispatcher_lease);
  const CpuRunResourceDemand resource_demand{
      dispatcher_lease.retained_memory_bytes(),
      ReadyTaskSubmission::default_resource_demand()};
  execution_service.execute_cpu_run(host, std::move(initial_submissions),
                                    static_cast<int>(plan.size()),
                                    resource_demand);
}

}  // namespace ps::compute
