#include "compute/dispatch/compute_task_submission.hpp"

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

#include "compute/dispatch/compute_dispatch_plan_builder.hpp"
#include "compute/request/compute_cache_policy.hpp"
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
#include "compute/execution/execution_service_test_probe.hpp"
#endif
#include "compute/execution/resource_demand_estimator.hpp"
#include "graph/graph_traversal_service.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Formats a graph node id with its name when still present.
 *
 * @param graph GraphModel used for node lookup.
 * @param node_id Node id being reported.
 * @return Human-readable dispatch error context.
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
 * @brief Creates a dispatch-stage GraphError exception pointer.
 *
 * @param graph GraphModel used to enrich the node label.
 * @param node_id Node whose dependent release failed.
 * @param detail Original dispatch exception detail.
 * @return Exception pointer carrying GraphErrc::ComputeError.
 * @throws std::bad_alloc if the wrapped diagnostic cannot allocate.
 * @note This preserves dispatch-stage context without relabeling
 * std::bad_alloc.
 */
std::exception_ptr dispatch_failure(const GraphModel& graph, int node_id,
                                    const std::string& detail) {
  return std::make_exception_ptr(
      GraphError(GraphErrc::ComputeError, "Dispatch stage after " +
                                              node_context(graph, node_id) +
                                              " failed: " + detail));
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
void settle_rejected_bootstrap(ExecutionTaskRuntime& task_runtime,
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
 * @brief Builds one Run-owned full HP execution submission plan.
 *
 * @param run_id Opaque namespace of the owning Run.
 * @param graph Graph used for planning and operation resolution.
 * @param traversal Traversal service used by plan construction.
 * @param node_id Requested target node.
 * @param available_devices Execution-runtime device labels.
 * @param publish_plan_inspection Whether construction immediately updates graph
 * diagnostics.
 * @param allow_reusable_cache Whether exact complete formal HP cache may
 * satisfy nodes before task population.
 * @throws GraphError or standard exceptions from planning and allocation.
 */
TaskSubmissionPlan::TaskSubmissionPlan(ComputeRunId run_id, GraphModel& graph,
                                       GraphTraversalService& traversal,
                                       int node_id,
                                       std::vector<Device> available_devices,
                                       bool publish_plan_inspection,
                                       bool allow_reusable_cache)
    : run_id_(run_id),
      graph_(graph),
      compute_plan_(
          ComputeDispatchPlanBuilder(traversal).build_high_precision_plan(
              graph, node_id, available_devices, publish_plan_inspection,
              allow_reusable_cache)),
      execution_order_(compute_plan_.planned_nodes),
      available_devices_(std::move(available_devices)),
      dependency_state_(execution_order_, compute_plan_.task_graph) {
  resolve_operations();
  temp_results_.resize(execution_order_.size());
  task_execution_states_ = std::vector<std::atomic<std::uint8_t>>(size());
  deferred_value_waits_.resize(size());
  for (auto& state : task_execution_states_) {
    state.store(static_cast<std::uint8_t>(TaskExecutionState::Pending),
                std::memory_order_relaxed);
  }
}

/** @copydoc TaskSubmissionPlan::RuntimeCompletionRecord::transfer_to_callback
 */
bool TaskSubmissionPlan::RuntimeCompletionRecord::
    transfer_to_callback() noexcept {
  std::uint8_t expected =
      static_cast<std::uint8_t>(RuntimeCompletionOwner::Plan);
  return owner_.compare_exchange_strong(
      expected, static_cast<std::uint8_t>(RuntimeCompletionOwner::Callback),
      std::memory_order_acq_rel, std::memory_order_acquire);
}

/** @copydoc TaskSubmissionPlan::RuntimeCompletionRecord::retire_plan_owned */
void TaskSubmissionPlan::RuntimeCompletionRecord::retire_plan_owned() noexcept {
  std::uint8_t expected =
      static_cast<std::uint8_t>(RuntimeCompletionOwner::Plan);
  if (owner_.compare_exchange_strong(
          expected, static_cast<std::uint8_t>(RuntimeCompletionOwner::Retired),
          std::memory_order_acq_rel, std::memory_order_acquire)) {
    retire_runtime();
  }
}

/** @copydoc TaskSubmissionPlan::RuntimeCompletionRecord::retire_callback_owned
 */
void TaskSubmissionPlan::RuntimeCompletionRecord::
    retire_callback_owned() noexcept {
  std::uint8_t expected =
      static_cast<std::uint8_t>(RuntimeCompletionOwner::Callback);
  if (owner_.compare_exchange_strong(
          expected, static_cast<std::uint8_t>(RuntimeCompletionOwner::Retired),
          std::memory_order_acq_rel, std::memory_order_acquire)) {
    retire_runtime();
  }
}

/** @copydoc TaskSubmissionPlan::RuntimeCompletionRecord::retire_runtime */
void TaskSubmissionPlan::RuntimeCompletionRecord::retire_runtime() noexcept {
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
  estimate.add_objects<Device>(
      static_cast<std::uint64_t>(execution_devices_.capacity()));
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
  estimate.add_objects<std::optional<ReadyFenceWaitRegistration>>(
      static_cast<std::uint64_t>(deferred_value_waits_.capacity()));
  estimate.add_objects<std::shared_ptr<RuntimeCompletionRecord>>(
      static_cast<std::uint64_t>(runtime_completion_records_.capacity()));
  estimate.add_objects<RuntimeCompletionRecord>(
      static_cast<std::uint64_t>(runtime_completion_records_.size()));
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(runtime_completion_records_.size()));
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(runtime_completion_records_.size()));
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
  estimate.add_objects<std::optional<OpImplementation>>(
      static_cast<std::uint64_t>(resolved_ops_.capacity()));
  for (const std::optional<OpImplementation>& implementation : resolved_ops_) {
    if (implementation.has_value()) {
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
      const std::uint64_t before_operation_key = estimate.bytes();
#endif
      estimate.add_string_payload(implementation->metadata.exclusive_key);
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
      testing::notify_retained_operation_string_charge_for_testing(
          testing::RetainedOperationStringOwner::FullPlanResolvedOperation,
          implementation->metadata.exclusive_key, before_operation_key,
          estimate.bytes());
#endif
      estimate.add_objects<std::string>(static_cast<std::uint64_t>(
          implementation->metadata.parameter_output_names.capacity()));
      for (const std::string& name :
           implementation->metadata.parameter_output_names) {
        estimate.add_string_payload(name);
      }
    }
  }
  estimate.add_objects<OperationExecutionConstraints>(
      static_cast<std::uint64_t>(operation_constraints_.capacity()));
  for (const OperationExecutionConstraints& constraints :
       operation_constraints_) {
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
    const std::uint64_t before_constraint_key = estimate.bytes();
#endif
    estimate.add_string_payload(constraints.exclusive_key);
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
    testing::notify_retained_operation_string_charge_for_testing(
        testing::RetainedOperationStringOwner::FullPlanExecutionConstraint,
        constraints.exclusive_key, before_constraint_key, estimate.bytes());
#endif
  }
  return estimate.bytes();
}

/**
 * @brief Discovers one validated initial ready identity set.
 *
 * @return Initial composite identities in this Run namespace.
 * @throws GraphError when no initial identity exists for a nonempty plan.
 * @throws std::overflow_error when task count exceeds runtime accounting.
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
        "Full HP task count exceeds runtime completion range.");
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
  const PlannedTask& task = compute_plan_.task_graph.tasks.at(task_index);
  const int trace_node_id = task.node_id;
  const std::size_t execution_index =
      static_cast<std::size_t>(dependency_state_.id_to_idx().at(trace_node_id));
  const Device device = execution_devices_.at(execution_index);
  ReadyTaskSubmission submission(
      lease, identity, trace_node_id, is_initial_ready,
      [](ComputeRunLease& ready_lease,
         const ComputeRunTaskIdentity& ready_identity,
         ExecutionTaskRuntime& task_runtime) {
        ready_lease.execute_task(ready_identity, task_runtime);
      },
      ExecutionTaskPriority::Normal, task_resource_demand_, device,
      std::move(operation_constraints_.at(task_index)));
  observe_task_ready(lease, identity, task, device);
  return submission;
}

/** @copydoc TaskSubmissionPlan::observe_task_ready */
void TaskSubmissionPlan::observe_task_ready(
    const ComputeRunLease& lease, const ComputeRunTaskIdentity& identity,
    const PlannedTask& task, Device device) const noexcept {
  const ComputeRunDescriptor& descriptor = lease.descriptor();
  const std::shared_ptr<ComputeRunObservationSink>& sink =
      descriptor.observation_sink();
  if (!sink || !sink->observes_task_semantics()) {
    return;
  }
  const ComputeRunObservationCoordinate coordinate =
      sink->reserve_causal_coordinate();
  const ComputeRunTaskReadyObservation observation{
      task.dependency_task_ids.data(),
      task.dependency_task_ids.size(),
      task.kind == PlannedTaskKind::Tile,
      task.output_roi.x,
      task.output_roi.y,
      task.output_roi.width,
      task.output_roi.height,
      device,
      task_resource_demand_.work_units,
      task_resource_demand_.retained_memory_bytes,
      task_resource_demand_.scratch_bytes,
      task_resource_demand_.ready_bytes};
  sink->on_task_ready(descriptor, identity, observation, coordinate);
}

/** @copydoc TaskSubmissionPlan::observe_task_terminal */
void TaskSubmissionPlan::observe_task_terminal(
    const ComputeRunLease& lease, const ComputeRunTaskIdentity& identity,
    ComputeRunTaskTerminalKind kind) const noexcept {
  const ComputeRunDescriptor& descriptor = lease.descriptor();
  const std::shared_ptr<ComputeRunObservationSink>& sink =
      descriptor.observation_sink();
  if (!sink || !sink->observes_task_semantics()) {
    return;
  }
  const ComputeRunObservationCoordinate coordinate =
      sink->reserve_causal_coordinate();
  sink->on_task_terminal(descriptor, identity, kind, coordinate);
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
 * @brief Submits all initial ready identities as runtime-owned callbacks.
 *
 * @param lease Matching Run lease copied into callbacks.
 * @param task_runtime Active execution batch.
 * @return Nothing.
 * @throws GraphError when no initial identity exists for a nonempty plan.
 * @throws std::overflow_error when planned count exceeds runtime integer
 * accounting.
 * @throws std::bad_alloc or runtime exceptions from submission.
 * @note Cancellation closes the publication gate and retires every completion
 * unit not yet transferred to a materialized callback. The caller retains the
 * separate bootstrap completion unit.
 */
void TaskSubmissionPlan::submit_initial_ready_tasks(
    const ComputeRunLease& lease, ExecutionTaskRuntime& task_runtime) {
  const std::vector<ComputeRunTaskIdentity> initial_identities =
      initial_ready_identities();

  if (!initialize_runtime_completion_ledger(lease, task_runtime)) {
    return;
  }
  log_initial_assignments(task_runtime);
  for (const ComputeRunTaskIdentity& identity : initial_identities) {
    (void)publish_runtime_callback(lease, identity, task_runtime);
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
 * @param task_runtime Active execution runtime.
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
                                      ExecutionTaskRuntime& task_runtime) {
  if (!contains_task_identity(identity)) {
    throw std::invalid_argument(
        "Task identity does not belong to this Run submission plan.");
  }
  (void)lease.observe_cancellation();
  const std::optional<ComputeRunTerminalOutcome> initial_terminal =
      lease.terminal_outcome();
  if (initial_terminal.has_value()) {
    const ComputeRunTaskTerminalKind kind =
        initial_terminal->kind == ComputeRunTerminalKind::Cancelled
            ? ComputeRunTaskTerminalKind::Cancelled
            : (initial_terminal->kind == ComputeRunTerminalKind::Succeeded
                   ? ComputeRunTaskTerminalKind::Succeeded
                   : ComputeRunTaskTerminalKind::Failed);
    observe_task_terminal(lease, identity, kind);
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
    observe_task_terminal(lease, identity, ComputeRunTaskTerminalKind::Failed);
    throw GraphError(GraphErrc::ComputeError,
                     "TaskSubmissionPlan has no owned task runner.");
  }

  const PlannedTask& task = compute_plan_.task_graph.tasks.at(task_id);
  const ExecutionTraceAction execute_action =
      task.kind == PlannedTaskKind::Tile ? ExecutionTraceAction::ExecuteTile
                                         : ExecutionTraceAction::Execute;
  task_runtime.log_event(execute_action, task.node_id);
  try {
    const NodeTaskRunner::TaskDependencyRelease dependency_release =
        task_runner_->run_task(task_id);
    if (dependency_release ==
            NodeTaskRunner::TaskDependencyRelease::CurrentTask &&
        defer_pending_value(task, identity, lease, task_runtime)) {
      return;
    }
    (void)lease.observe_cancellation();
    if (!lease.terminal_outcome().has_value()) {
      if (dependency_release ==
          NodeTaskRunner::TaskDependencyRelease::CompleteTiledNode) {
        release_tiled_node_dependents(task.node_id, lease, task_runtime);
      } else if (dependency_release ==
                 NodeTaskRunner::TaskDependencyRelease::CurrentTask) {
        release_dependents(task.task_id, task.node_id, lease, task_runtime);
      }
    }
    task_execution_states_.at(task_id).store(
        static_cast<std::uint8_t>(TaskExecutionState::Completed),
        std::memory_order_release);
    observe_task_terminal(lease, identity,
                          ComputeRunTaskTerminalKind::Succeeded);
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    task_execution_states_.at(task_id).store(
        static_cast<std::uint8_t>(TaskExecutionState::Failed),
        std::memory_order_release);
    observe_task_terminal(lease, identity, ComputeRunTaskTerminalKind::Failed);
    try {
      task_runtime.log_event(ExecutionTraceAction::RethrowException,
                             task.node_id);
    } catch (...) {
    }
    std::rethrow_exception(failure);
  }
}

/** @copydoc TaskSubmissionPlan::defer_pending_value */
bool TaskSubmissionPlan::defer_pending_value(
    const PlannedTask& task, const ComputeRunTaskIdentity& identity,
    const ComputeRunLease& lease, ExecutionTaskRuntime& task_runtime) {
  const auto index = dependency_state_.id_to_idx().find(task.node_id);
  if (index == dependency_state_.id_to_idx().end()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Completed task has no temporary result index.");
  }
  std::optional<NodeOutput>& result =
      temp_results_.at(static_cast<std::size_t>(index->second));
  if (!result.has_value() || !result->has_image_value()) {
    return false;
  }

  const ReadyFenceSnapshot observed =
      result->image_value().ready_fence().poll();
  if (observed.state() == ReadyFenceState::Ready) {
    return false;
  }
  if (observed.state() != ReadyFenceState::Pending) {
    throw ReadyFenceAccessError(observed);
  }

  std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
  if (publication_closed_ || lease.terminal_outcome().has_value()) {
    return false;
  }
  const int task_id = task.task_id;
  std::shared_ptr<ReadyFenceExecutor> executor =
      task_runtime.make_ready_fence_executor();
  ComputeRunLease continuation_lease = lease;
  ReadyFence::Callback callback =
      [lease = std::move(continuation_lease), identity,
       runtime = &task_runtime](ReadyFenceSnapshot snapshot) mutable {
        lease.complete_deferred_value(identity, *runtime, std::move(snapshot));
      };

  task_execution_states_.at(static_cast<std::size_t>(task_id))
      .store(static_cast<std::uint8_t>(TaskExecutionState::AwaitingValue),
             std::memory_order_release);
  bool completion_counted = false;
  try {
    task_runtime.inc_tasks_to_complete(1);
    completion_counted = true;
    ReadyFenceWaitRegistration registration =
        result->image_value().ready_fence().async_wait(std::move(executor),
                                                       std::move(callback));
    deferred_value_waits_.at(static_cast<std::size_t>(task_id))
        .emplace(std::move(registration));
  } catch (...) {
    task_execution_states_.at(static_cast<std::size_t>(task_id))
        .store(static_cast<std::uint8_t>(TaskExecutionState::Failed),
               std::memory_order_release);
    if (completion_counted) {
      task_runtime.dec_tasks_to_complete();
    }
    throw;
  }
  return true;
}

/** @copydoc TaskSubmissionPlan::complete_deferred_value */
void TaskSubmissionPlan::complete_deferred_value(
    const ComputeRunTaskIdentity& identity, const ComputeRunLease& lease,
    ExecutionTaskRuntime& task_runtime, ReadyFenceSnapshot snapshot) {
  if (!contains_task_identity(identity)) {
    throw std::invalid_argument(
        "Deferred Value identity does not belong to this submission plan.");
  }
  const int task_id = static_cast<int>(identity.local_task_id().value());
  std::uint8_t expected =
      static_cast<std::uint8_t>(TaskExecutionState::AwaitingValue);
  if (!task_execution_states_.at(static_cast<std::size_t>(task_id))
           .compare_exchange_strong(
               expected,
               static_cast<std::uint8_t>(TaskExecutionState::CompletingValue),
               std::memory_order_acq_rel, std::memory_order_acquire)) {
    throw std::logic_error(
        "Deferred Value continuation entered more than once.");
  }

  const PlannedTask& task =
      compute_plan_.task_graph.tasks.at(static_cast<std::size_t>(task_id));
  try {
    if (snapshot.state() != ReadyFenceState::Ready) {
      throw ReadyFenceAccessError(std::move(snapshot));
    }
    const auto index = dependency_state_.id_to_idx().find(task.node_id);
    if (index == dependency_state_.id_to_idx().end()) {
      throw GraphError(GraphErrc::ComputeError,
                       "Deferred task has no temporary result index.");
    }
    std::optional<NodeOutput>& result =
        temp_results_.at(static_cast<std::size_t>(index->second));
    if (!result.has_value() || !result->has_image_value()) {
      throw GraphError(GraphErrc::ComputeError,
                       "Deferred task lost its pending Value result.");
    }
    (void)lease.observe_cancellation();
    if (!lease.terminal_outcome().has_value()) {
      release_dependents(task.task_id, task.node_id, lease, task_runtime);
    }
    task_execution_states_.at(static_cast<std::size_t>(task_id))
        .store(static_cast<std::uint8_t>(TaskExecutionState::Completed),
               std::memory_order_release);
    observe_task_terminal(lease, identity,
                          ComputeRunTaskTerminalKind::Succeeded);
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    task_execution_states_.at(static_cast<std::size_t>(task_id))
        .store(static_cast<std::uint8_t>(TaskExecutionState::Failed),
               std::memory_order_release);
    observe_task_terminal(lease, identity, ComputeRunTaskTerminalKind::Failed);
    try {
      task_runtime.log_event(ExecutionTraceAction::RethrowException,
                             task.node_id);
    } catch (...) {
    }
    std::rethrow_exception(failure);
  }
}

/** @copydoc TaskSubmissionPlan::release_tiled_node_dependents */
void TaskSubmissionPlan::release_tiled_node_dependents(
    int node_id, const ComputeRunLease& lease,
    ExecutionTaskRuntime& task_runtime) {
  bool found_tile = false;
  for (const PlannedTask& sibling : compute_plan_.task_graph.tasks) {
    if (sibling.node_id != node_id || sibling.kind != PlannedTaskKind::Tile) {
      continue;
    }
    found_tile = true;
    release_dependents(sibling.task_id, node_id, lease, task_runtime);
  }
  if (!found_tile) {
    throw GraphError(GraphErrc::ComputeError,
                     "Tiled dependency release found no sibling tasks.");
  }
}

/** @copydoc TaskSubmissionPlan::resolve_operations */
void TaskSubmissionPlan::resolve_operations() {
  resolved_ops_.resize(execution_order_.size());
  execution_devices_.assign(execution_order_.size(), Device::CPU);
  operation_constraints_.resize(compute_plan_.task_graph.tasks.size());
  task_resource_demand_ = ReadyTaskSubmission::default_resource_demand();
  for (std::size_t i = 0; i < execution_order_.size(); ++i) {
    const auto& node = graph_.node(execution_order_[i]);
    const auto planned_work = std::find_if(
        compute_plan_.planned_work.begin(), compute_plan_.planned_work.end(),
        [&](const PlannedNodeWork& work) {
          return work.node_id == execution_order_[i];
        });
    if (planned_work == compute_plan_.planned_work.end() ||
        !planned_work->operation_route.has_value()) {
      continue;
    }

    const PlannedOperationRoute& route = *planned_work->operation_route;
    const auto implementation = OpRegistry::instance().select_implementation(
        node.type, node.subtype, available_devices_,
        ComputeIntent::GlobalHighPrecision);
    if (!implementation ||
        implementation->implementation_identity !=
            route.implementation_identity ||
        implementation->metadata.device_preference != route.device ||
        implementation->is_tiled() != route.tiled) {
      continue;
    }
    execution_devices_[i] = implementation->metadata.device_preference;
    resolved_ops_[i] = *implementation;
    for (const int task_id : planned_work->task_ids) {
      operation_constraints_.at(static_cast<std::size_t>(task_id)) =
          OperationExecutionConstraints{
              implementation->implementation_identity,
              implementation->metadata.reentrant,
              implementation->metadata.maximum_parallelism,
              implementation->metadata.exclusive_key,
          };
    }
    task_resource_demand_.retained_memory_bytes =
        std::max(task_resource_demand_.retained_memory_bytes,
                 implementation->metadata.retained_memory_bytes);
    task_resource_demand_.scratch_bytes =
        std::max(task_resource_demand_.scratch_bytes,
                 implementation->metadata.scratch_bytes);
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
    ExecutionTaskRuntime& task_runtime) {
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
        (void)publish_runtime_callback(lease, identity, task_runtime);
      }
    }
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::out_of_range& error) {
    std::rethrow_exception(dispatch_failure(
        graph_, current_node_id, "out_of_range: " + std::string(error.what())));
  } catch (const std::exception& error) {
    std::rethrow_exception(
        dispatch_failure(graph_, current_node_id, error.what()));
  } catch (...) {
    std::rethrow_exception(
        dispatch_failure(graph_, current_node_id, "unknown exception"));
  }
}

/** @copydoc TaskSubmissionPlan::initialize_runtime_completion_ledger */
bool TaskSubmissionPlan::initialize_runtime_completion_ledger(
    const ComputeRunLease& lease, ExecutionTaskRuntime& task_runtime) {
  (void)lease.observe_cancellation();
  std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
  if (runtime_completion_initialized_) {
    throw std::logic_error(
        "TaskSubmissionPlan runtime completion ledger is already initialized.");
  }
  if (publication_closed_ || lease.terminal_outcome().has_value()) {
    publication_closed_ = true;
    return false;
  }

  std::vector<std::shared_ptr<RuntimeCompletionRecord>> records;
  records.reserve(size());
  for (std::size_t index = 0U; index < size(); ++index) {
    records.push_back(std::make_shared<RuntimeCompletionRecord>(task_runtime));
  }
  task_runtime.inc_tasks_to_complete(static_cast<int>(size()));
  runtime_completion_records_ = std::move(records);
  runtime_completion_initialized_ = true;

  if (lease.terminal_outcome().has_value()) {
    publication_closed_ = true;
    for (const auto& record : runtime_completion_records_) {
      record->retire_plan_owned();
    }
    return false;
  }
  return true;
}

/** @copydoc TaskSubmissionPlan::publish_runtime_callback */
bool TaskSubmissionPlan::publish_runtime_callback(
    const ComputeRunLease& lease, const ComputeRunTaskIdentity& identity,
    ExecutionTaskRuntime& task_runtime) {
  (void)lease.observe_cancellation();
  std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
  if (publication_closed_ || lease.terminal_outcome().has_value()) {
    return false;
  }
  if (!runtime_completion_initialized_) {
    throw std::logic_error(
        "Runtime callback publication requires an initialized completion "
        "ledger.");
  }
  const std::size_t task_index =
      static_cast<std::size_t>(identity.local_task_id().value());
  const std::shared_ptr<RuntimeCompletionRecord>& record =
      runtime_completion_records_.at(task_index);
  auto token = std::make_shared<RuntimeCompletionToken>(record);
  ComputeRunLease callback_lease = lease;
  ExecutionTaskRuntime::Task callback = [lease = std::move(callback_lease),
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
  std::vector<std::optional<ReadyFenceWaitRegistration>> retired_waits;
  try {
    {
      std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
      if (publication_closed_) {
        return;
      }
      publication_closed_ = true;
      for (const auto& record : runtime_completion_records_) {
        record->retire_plan_owned();
      }
      retired_waits.swap(deferred_value_waits_);
    }
    for (auto& wait : retired_waits) {
      if (wait.has_value()) {
        (void)wait->cancel();
      }
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
    ExecutionTaskRuntime& task_runtime) const {
  for (int task_id : submitted_initial_task_ids_) {
    if (task_id < 0 || task_id >= static_cast<int>(size())) {
      continue;
    }
    const int node_id = compute_plan_.task_graph.tasks[task_id].node_id;
    if (std::find(execution_order_.begin(), execution_order_.end(), node_id) !=
        execution_order_.end()) {
      task_runtime.log_event(ExecutionTraceAction::AssignInitial, node_id);
    }
  }
}

/**
 * @brief Establishes one execution epoch and submits a leased bootstrap.
 *
 * @param graph Graph used to validate exact complete empty-plan target output.
 * @param task_runtime Runtime receiving the empty epoch batch and callbacks.
 * @param node_id Target node id.
 * @param plan Run-owned plan.
 * @param dispatcher_lease Lease copied into bootstrap callback.
 * @return Nothing.
 * @throws GraphError for an empty plan without ComputeCachePolicy-approved
 * complete target output.
 * @throws Execution-runtime or task exceptions unchanged.
 * @note The only ExecutionTaskHandle batch is empty; full HP work uses owned
 * callbacks. Partial persistent Region state never authorizes the early
 * return.
 */
void dispatch_planned_tasks(GraphModel& graph,
                            ExecutionTaskRuntime& task_runtime, int node_id,
                            TaskSubmissionPlan& plan,
                            const ComputeRunLease& dispatcher_lease) {
  if (plan.empty() && graph.has_node(node_id)) {
    if (!ComputeCachePolicy::has_reusable_output(graph.node(node_id))) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Planned dispatch produced no executable tasks for node " +
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
 * @param graph Graph used only for exact complete empty-plan target validation.
 * @param execution_service Injected process CPU service.
 * @param host Active Graph observation context.
 * @param node_id Requested target node.
 * @param plan Run-owned dispatcher submission plan.
 * @param dispatcher_lease Matching Run lease.
 * @return Nothing after service settlement.
 * @throws GraphError for an empty plan without ComputeCachePolicy-approved
 * complete target output.
 * @throws Service setup or exact task exceptions unchanged.
 * @note Partial persistent Region state never authorizes the early return.
 * The complete shared Run estimate is frozen before initial submission
 * materialization moves each task's already-charged constraint-key allocation
 * out of the plan.
 */
void dispatch_planned_tasks(GraphModel& graph,
                            ExecutionService& execution_service,
                            ExecutionHostContext& host,
                            const std::string& execution_type, int node_id,
                            TaskSubmissionPlan& plan,
                            const ComputeRunLease& dispatcher_lease) {
  if (plan.empty() && graph.has_node(node_id)) {
    if (!ComputeCachePolicy::has_reusable_output(graph.node(node_id))) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Planned dispatch produced no executable tasks for node " +
              std::to_string(node_id) +
              " and the target has no reusable high-precision output.");
    }
    return;
  }

  const CpuRunResourceDemand resource_demand{
      dispatcher_lease.retained_memory_bytes(), plan.task_resource_demand()};
  std::vector<ReadyTaskSubmission> initial_submissions =
      plan.make_initial_ready_submissions(dispatcher_lease);
  execution_service.execute_run(host, execution_type,
                                std::move(initial_submissions),
                                static_cast<int>(plan.size()), resource_demand);
}

}  // namespace ps::compute
