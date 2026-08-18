#include "compute/dirty/dirty_execution_common.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "compute/compute_geometry.hpp"
#include "compute/execution/resource_demand_estimator.hpp"
#include "compute/request/compute_cache_policy.hpp"
#include "core/region_image_adapter.hpp"
#include "runtime/graph_runtime.hpp"

namespace ps::compute {

/**
 * @brief Complete unpublished source-first dirty dispatch state.
 *
 * @throws std::bad_alloc when copied request/callable or inline dependency
 * storage allocates.
 * @note The normalized observation lease has a stable heap address referenced
 * by request. Both service roots remain reserved together until execute() or
 * rollback consumes the state.
 */
struct PreparedDirtySourceFirstRunState final {
  /**
   * @brief Captures one dirty request and normalizes its observation lease.
   * @param source_request Complete route and planning inputs.
   * @param task_callable Owned type-erased task function.
   * @throws std::bad_alloc when copied request/callable storage allocates.
   * @throws std::logic_error if a direct Run is already terminal.
   */
  PreparedDirtySourceFirstRunState(
      const DirtySourceFirstRunRequest& source_request,
      std::function<void(int)> task_callable)
      : request(source_request), run_task(std::move(task_callable)) {
    if (request.run_lease != nullptr) {
      observation_lease.emplace(*request.run_lease);
    } else if (request.run != nullptr) {
      observation_lease.emplace(request.run->acquire_lease());
    }
    request.run_lease =
        observation_lease.has_value() ? &*observation_lease : nullptr;
  }

  /** @brief Copied pointers/options with normalized lease address. */
  DirtySourceFirstRunRequest request;
  /** @brief Strong observation lease retained across both phase roots. */
  std::optional<ComputeRunLease> observation_lease;
  /** @brief Inline task callable; service preparation transfers copies. */
  std::function<void(int)> run_task;
  /** @brief Complete unpublished source phase, when nonempty. */
  PreparedExecutionRun source_run;
  /** @brief Complete unpublished downstream phase, when nonempty. */
  PreparedExecutionRun downstream_run;
  /**
   * @brief Source context retained until its cancellation callback returns.
   * @note This owner prevents the context from destroying its own synchronized
   * cancellation registration recursively inside the registered callback.
   */
  std::shared_ptr<DirtyReadyTaskContext> source_context;
  /**
   * @brief Downstream context retained until its cancellation callback returns.
   * @note Normal phase completion releases this owner immediately after
   * service settlement instead of extending it through later request work.
   */
  std::shared_ptr<DirtyReadyTaskContext> downstream_context;
  /** @brief Preallocated inline downstream dependency counters. */
  std::unique_ptr<TaskDependencyState> inline_dependency_state;
  /** @brief Preallocated inline ready LIFO with total-task capacity. */
  std::vector<int> inline_ready_stack;
};

namespace {

/**
 * @brief Rejects one dirty boundary after accepted Run cancellation.
 * @param state Prepared dirty state with normalized observer.
 * @return Nothing while cancellation has not won.
 * @throws GraphError after accepted cancellation.
 */
void observe_prepared_dirty_cancellation(
    const PreparedDirtySourceFirstRunState& state) {
  std::optional<ComputeRunCancellationReason> reason;
  if (state.request.run_lease != nullptr) {
    reason = state.request.run_lease->observe_cancellation();
  } else if (state.request.run != nullptr) {
    reason = state.request.run->observe_cancellation();
  }
  if (reason.has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ComputeRun cancelled during dirty dispatch.");
  }
}

/**
 * @brief Computes complete shared demand for one prepared dirty phase.
 * @param request Prepared source-first request.
 * @param task_ids Exact phase task ids.
 * @return Checked retained bytes.
 * @throws GraphError when checked arithmetic overflows.
 * @throws Any phase callback exception unchanged.
 */
std::uint64_t prepared_dirty_phase_retained_bytes(
    const DirtySourceFirstRunRequest& request,
    const std::vector<int>& task_ids) {
  RetainedMemoryEstimator estimate("dirty phase retained demand");
  estimate.add_bytes(request.additional_shared_retained_memory_bytes);
  if (request.phase_shared_retained_memory_bytes) {
    estimate.add_bytes(request.phase_shared_retained_memory_bytes(task_ids));
  }
  return estimate.bytes();
}

/**
 * @brief Validates that one inline dirty task did not leave Pending output.
 * @param request Complete prepared request containing the staging observer.
 * @param task_id Dense planned task id that just returned.
 * @return Nothing when no staged output exists or its planned output is Ready.
 * @throws GraphError for a missing output authority or malformed output.
 * @throws ReadyFenceAccessError when the staged image is not Ready.
 * @throws std::out_of_range for an invalid task id.
 * @note Inline execution has no any-thread continuation executor. It therefore
 * fails closed instead of blocking the caller or releasing dependents early.
 */
void validate_inline_dirty_task_output_ready(
    const DirtySourceFirstRunRequest& request, int task_id) {
  if (request.snapshot_task_output == nullptr) {
    return;
  }
  const PlannedTask& task = request.compute_plan->task_graph.tasks.at(
      static_cast<std::size_t>(task_id));
  std::optional<NodeOutput> output =
      request.snapshot_task_output(request.task_output_context, task.node_id);
  if (!output.has_value()) {
    return;
  }
  if (task.kind == PlannedTaskKind::Tile && !output->has_image_value()) {
    return;
  }
  const auto authority =
      std::find_if(request.compute_plan->planned_work.begin(),
                   request.compute_plan->planned_work.end(),
                   [&task](const PlannedNodeWork& work) {
                     return work.node_id == task.node_id;
                   });
  if (authority == request.compute_plan->planned_work.end() ||
      !authority->output_authority.has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Inline dirty task is missing frozen output authority.");
  }
  validate_planned_output(*output, *authority->output_authority,
                          PlannedOutputReadiness::RequireReady);
}

/**
 * @brief Compares complete immutable publication and indexed binding identity.
 * @param lhs First valid canonical or generic Value.
 * @param rhs Second valid canonical or generic Value.
 * @return True when revision, producer, representation, binding count, and
 * every indexed StorageBinding match.
 * @throws std::logic_error if either supposedly valid Value violates its own
 * immutable representation or binding invariants.
 * @note Readiness is deliberately excluded because a Pending publication and
 * its Ready continuation retain the same identities and binding facts.
 */
bool same_value_publication_identity(const Value& lhs, const Value& rhs) {
  if (!lhs.valid() || !rhs.valid() || lhs.revision_id() != rhs.revision_id() ||
      lhs.producer_identity() != rhs.producer_identity() ||
      lhs.representation_kind() != rhs.representation_kind() ||
      lhs.buffer_count() != rhs.buffer_count()) {
    return false;
  }
  for (std::size_t index = 0U; index < lhs.buffer_count(); ++index) {
    if (lhs.storage_binding(index) != rhs.storage_binding(index)) {
      return false;
    }
  }
  return true;
}

}  // namespace

/** @copydoc PreparedDirtySourceFirstRun::PreparedDirtySourceFirstRun */
PreparedDirtySourceFirstRun::PreparedDirtySourceFirstRun() noexcept = default;

/** @copydoc PreparedDirtySourceFirstRun::PreparedDirtySourceFirstRun */
PreparedDirtySourceFirstRun::PreparedDirtySourceFirstRun(
    std::unique_ptr<PreparedDirtySourceFirstRunState> state) noexcept
    : state_(std::move(state)) {}

/** @copydoc PreparedDirtySourceFirstRun::PreparedDirtySourceFirstRun */
PreparedDirtySourceFirstRun::PreparedDirtySourceFirstRun(
    PreparedDirtySourceFirstRun&& other) noexcept = default;  // NOLINT

/** @copydoc PreparedDirtySourceFirstRun::operator= */
PreparedDirtySourceFirstRun& PreparedDirtySourceFirstRun::operator=(
    PreparedDirtySourceFirstRun&& other) noexcept {
  if (this != &other) {
    if (state_) {
      std::terminate();
    }
    state_ = std::move(other.state_);
  }
  return *this;
}

/** @copydoc PreparedDirtySourceFirstRun::~PreparedDirtySourceFirstRun */
PreparedDirtySourceFirstRun::~PreparedDirtySourceFirstRun() noexcept = default;

/** @copydoc prepare_dirty_source_first */
PreparedDirtySourceFirstRun prepare_dirty_source_first(
    const DirtySourceFirstRunRequest& request,
    std::function<void(int)> run_task, std::uint64_t run_task_capture_bytes) {
  if (!run_task || request.compute_plan == nullptr ||
      request.source_task_ids == nullptr ||
      request.downstream_task_ids == nullptr ||
      request.task_devices == nullptr || request.task_constraints == nullptr ||
      request.task_devices->size() !=
          request.compute_plan->task_graph.tasks.size() ||
      request.task_constraints->size() !=
          request.compute_plan->task_graph.tasks.size()) {
    throw std::invalid_argument(
        "Dirty source-first preparation requires complete plan and task "
        "inputs.");
  }

  auto state = std::make_unique<PreparedDirtySourceFirstRunState>(
      request, std::move(run_task));
  observe_prepared_dirty_cancellation(*state);
  const ComputePlan& compute_plan = *state->request.compute_plan;
  const std::vector<int>& source_task_ids = *state->request.source_task_ids;
  const std::vector<int>& downstream_task_ids =
      *state->request.downstream_task_ids;

  if (state->request.execution_service != nullptr) {
    if (state->request.host == nullptr || state->request.run == nullptr ||
        state->request.run_lease == nullptr) {
      throw std::invalid_argument(
          "Dirty process-service preparation requires host, Run, and lease.");
    }
    const std::uint64_t run_task_retained_memory_bytes =
        owned_callable_retained_memory_bytes(run_task_capture_bytes);
    std::function<void(int)> owned_run_task = std::move(state->run_task);
    const ComputeRunLease phase_lease = *state->request.run_lease;

    if (!source_task_ids.empty()) {
      state->source_context = std::make_shared<DirtyReadyTaskContext>(
          compute_plan, state->request.selection, source_task_ids,
          *state->request.task_devices, *state->request.task_constraints,
          state->request.task_operation_resource_demand, owned_run_task,
          run_task_retained_memory_bytes, phase_lease, false,
          ExecutionTaskPriority::High, state->request.task_output_context,
          state->request.snapshot_task_output);
      state->source_context->install_cancellation_notification();
      RetainedMemoryEstimator source_phase_retained(
          "dirty source phase retained demand");
      source_phase_retained.add_bytes(
          prepared_dirty_phase_retained_bytes(state->request, source_task_ids));
      source_phase_retained.add_bytes(run_task_retained_memory_bytes);
      const CpuRunResourceDemand source_resource_demand =
          state->source_context->run_resource_demand(
              source_phase_retained.bytes());
      std::vector<ReadyTaskSubmission> source_submissions =
          state->source_context->make_submissions(source_task_ids, true);
      state->source_run = state->request.execution_service->prepare_run(
          *state->request.host, state->request.execution_type,
          std::move(source_submissions),
          static_cast<int>(source_task_ids.size()), source_resource_demand);
    }

    if (!downstream_task_ids.empty()) {
      std::vector<int> initial_downstream_ids;
      if (state->request.selection != nullptr) {
        initial_downstream_ids =
            state->request.selection->initial_downstream_task_ids;
      } else {
        TaskGraphReadyChecker ready_checker;
        initial_downstream_ids = ready_checker.initial_ready_task_ids(
            compute_plan.task_graph, &downstream_task_ids);
      }
      state->downstream_context = make_dirty_context_and_release_outer_callable(
          owned_run_task, [&](std::function<void(int)> transferred_run_task) {
            return std::make_shared<DirtyReadyTaskContext>(
                compute_plan, state->request.selection, downstream_task_ids,
                *state->request.task_devices, *state->request.task_constraints,
                state->request.task_operation_resource_demand,
                std::move(transferred_run_task), run_task_retained_memory_bytes,
                phase_lease, true,
                state->request.intent == ComputeIntent::RealTimeUpdate
                    ? ExecutionTaskPriority::High
                    : ExecutionTaskPriority::Normal,
                state->request.task_output_context,
                state->request.snapshot_task_output);
          });
      state->downstream_context->install_cancellation_notification();
      const CpuRunResourceDemand downstream_resource_demand =
          state->downstream_context->run_resource_demand(
              prepared_dirty_phase_retained_bytes(state->request,
                                                  downstream_task_ids));
      std::vector<ReadyTaskSubmission> downstream_submissions =
          state->downstream_context->make_submissions(initial_downstream_ids,
                                                      true);
      state->downstream_run = state->request.execution_service->prepare_run(
          *state->request.host, state->request.execution_type,
          std::move(downstream_submissions),
          static_cast<int>(downstream_task_ids.size()),
          downstream_resource_demand);
    }
    return PreparedDirtySourceFirstRun(std::move(state));
  }

  state->inline_dependency_state =
      state->request.selection != nullptr
          ? std::make_unique<TaskDependencyState>(
                compute_plan.execution_order, compute_plan.task_graph,
                downstream_task_ids,
                state->request.selection->dependency_task_ids)
          : std::make_unique<TaskDependencyState>(compute_plan.execution_order,
                                                  compute_plan.task_graph,
                                                  downstream_task_ids);
  std::vector<int> initial_downstream_ids;
  if (state->request.selection != nullptr) {
    initial_downstream_ids =
        state->request.selection->initial_downstream_task_ids;
  } else {
    TaskGraphReadyChecker ready_checker;
    initial_downstream_ids = ready_checker.initial_ready_task_ids(
        compute_plan.task_graph, &downstream_task_ids);
  }
  state->inline_ready_stack.reserve(downstream_task_ids.size());
  state->inline_ready_stack.assign(initial_downstream_ids.rbegin(),
                                   initial_downstream_ids.rend());
  return PreparedDirtySourceFirstRun(std::move(state));
}

/** @copydoc PreparedDirtySourceFirstRun::execute */
void PreparedDirtySourceFirstRun::execute() {
  if (!state_) {
    throw std::invalid_argument(
        "Dirty source-first execution requires active preparation.");
  }
  std::unique_ptr<PreparedDirtySourceFirstRunState> state = std::move(state_);
  observe_prepared_dirty_cancellation(*state);

  if (state->request.execution_service != nullptr) {
    if (state->source_run.active()) {
      state->request.execution_service->execute_prepared_run(
          std::move(state->source_run));
      state->source_context.reset();
      observe_prepared_dirty_cancellation(*state);
    }
    if (state->request.before_downstream) {
      observe_prepared_dirty_cancellation(*state);
      state->request.before_downstream();
      observe_prepared_dirty_cancellation(*state);
    }
    if (state->downstream_run.active()) {
      state->request.execution_service->execute_prepared_run(
          std::move(state->downstream_run));
      state->downstream_context.reset();
      observe_prepared_dirty_cancellation(*state);
    }
    return;
  }

  for (int source_task_id : *state->request.source_task_ids) {
    observe_prepared_dirty_cancellation(*state);
    state->run_task(source_task_id);
    validate_inline_dirty_task_output_ready(state->request, source_task_id);
    observe_prepared_dirty_cancellation(*state);
  }
  if (state->request.before_downstream) {
    observe_prepared_dirty_cancellation(*state);
    state->request.before_downstream();
    observe_prepared_dirty_cancellation(*state);
  }
  while (!state->inline_ready_stack.empty()) {
    const int task_id = state->inline_ready_stack.back();
    state->inline_ready_stack.pop_back();
    observe_prepared_dirty_cancellation(*state);
    state->run_task(task_id);
    validate_inline_dirty_task_output_ready(state->request, task_id);
    observe_prepared_dirty_cancellation(*state);
    std::vector<int> ready_ids =
        state->inline_dependency_state->release_dependents(task_id);
    for (auto iterator = ready_ids.rbegin(); iterator != ready_ids.rend();
         ++iterator) {
      state->inline_ready_stack.push_back(*iterator);
    }
  }
}

/** @copydoc DirtyNodeSynchronization::DirtyNodeSynchronization */
DirtyNodeSynchronization::DirtyNodeSynchronization(
    const std::vector<int>& node_ids) {
  node_mutexes_.reserve(node_ids.size());
  for (int node_id : node_ids) {
    if (node_mutexes_.find(node_id) == node_mutexes_.end()) {
      node_mutexes_.emplace(node_id, std::make_unique<std::mutex>());
    }
  }
}

/** @copydoc DirtyNodeSynchronization::mutex_for */
std::mutex& DirtyNodeSynchronization::mutex_for(int node_id) const {
  return *node_mutexes_.at(node_id);
}

/** @copydoc DirtyNodeSynchronization::retained_memory_bytes */
std::uint64_t DirtyNodeSynchronization::retained_memory_bytes() const {
  RetainedMemoryEstimator estimate("DirtyNodeSynchronization");
  const std::uint64_t bucket_count =
      static_cast<std::uint64_t>(node_mutexes_.bucket_count());
  const std::uint64_t node_count =
      static_cast<std::uint64_t>(node_mutexes_.size());
  estimate.add_objects<DirtyNodeSynchronization>();
  estimate.add_shared_control_block();
  estimate.add_objects<void*>(bucket_count);
  estimate.add_objects<decltype(node_mutexes_)::value_type>(node_count);
  estimate.add_objects<void*>(node_count);
  estimate.add_objects<void*>(node_count);
  estimate.add_objects<std::mutex>(node_count);
  return estimate.bytes();
}

/** @copydoc DirtyReadyTaskContext::DirtyReadyTaskContext */
struct DirtyReadyTaskContext::DeferredValueWait final {
  /** @brief Move-only cancellation authority for the registered fence wait. */
  ReadyFenceWaitRegistration registration;
  /** @brief Runtime owning the dynamically added logical completion unit. */
  ExecutionTaskRuntime* runtime = nullptr;
  /** @brief Exact-once ownership of that dynamically added unit. */
  std::atomic<bool> completion_outstanding{false};
};

/** @copydoc DirtyReadyTaskContext::DirtyReadyTaskContext */
DirtyReadyTaskContext::DirtyReadyTaskContext(
    const ComputePlan& compute_plan, const DirtyTaskSelectionOverlay* selection,
    const std::vector<int>& active_task_ids,
    const std::vector<Device>& task_devices,
    const std::vector<OperationExecutionConstraints>& task_constraints,
    ReadyTaskResourceDemand task_operation_resource_demand,
    std::function<void(int)> run_task,
    std::uint64_t run_task_retained_memory_bytes, ComputeRunLease lease,
    bool release_dependents, ExecutionTaskPriority priority,
    const void* task_output_context,
    DirtyTaskOutputSnapshot snapshot_task_output)
    : compute_plan_(compute_plan),
      selection_(selection
                     ? std::optional<DirtyTaskSelectionOverlay>(*selection)
                     : std::nullopt),
      active_task_ids_(active_task_ids),
      task_devices_(task_devices),
      task_constraints_(task_constraints),
      task_operation_resource_demand_(task_operation_resource_demand),
      active_task_id_set_(active_task_ids.begin(), active_task_ids.end()),
      run_task_(std::move(run_task)),
      run_task_retained_memory_bytes_(run_task_retained_memory_bytes),
      lease_(std::move(lease)),
      release_dependents_(release_dependents),
      priority_(priority),
      task_output_context_(task_output_context),
      snapshot_task_output_(snapshot_task_output) {
  if (!run_task_) {
    throw std::invalid_argument(
        "DirtyReadyTaskContext requires an owned task callable.");
  }
  if (task_devices_.size() != compute_plan_.task_graph.tasks.size()) {
    throw std::invalid_argument(
        "DirtyReadyTaskContext requires one device per planned task.");
  }
  if (task_constraints_.size() != compute_plan_.task_graph.tasks.size()) {
    throw std::invalid_argument(
        "DirtyReadyTaskContext requires one operation constraint per planned "
        "task.");
  }
  if (selection_) {
    dependency_state_ = std::make_unique<TaskDependencyState>(
        compute_plan_.execution_order, compute_plan_.task_graph,
        active_task_ids_, selection_->dependency_task_ids);
  } else {
    dependency_state_ = std::make_unique<TaskDependencyState>(
        compute_plan_.execution_order, compute_plan_.task_graph,
        active_task_ids_);
  }
  task_states_.assign(compute_plan_.task_graph.tasks.size(),
                      static_cast<std::uint8_t>(TaskState::Pending));
  deferred_value_waits_.resize(compute_plan_.task_graph.tasks.size());
  for (int task_id : active_task_ids_) {
    if (task_id < 0 ||
        task_id >= static_cast<int>(deferred_value_waits_.size())) {
      throw std::invalid_argument(
          "DirtyReadyTaskContext received an invalid active task id.");
    }
    deferred_value_waits_[static_cast<std::size_t>(task_id)] =
        std::make_unique<DeferredValueWait>();
  }
}

/** @copydoc DirtyReadyTaskContext::install_cancellation_notification */
void DirtyReadyTaskContext::install_cancellation_notification() {
  std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
  if (cancellation_notification_installed_) {
    throw std::logic_error(
        "Dirty cancellation notification was installed more than once.");
  }
  cancellation_notification_installed_ = true;
  const std::weak_ptr<DirtyReadyTaskContext> weak = weak_from_this();
  cancellation_registration_ = lease_.register_cancellation_notification(
      [weak](ComputeRunCancellationReason) {
        if (const std::shared_ptr<DirtyReadyTaskContext> self = weak.lock()) {
          self->close_publication();
        }
      });
}

/** @copydoc DirtyReadyTaskContext::~DirtyReadyTaskContext */
DirtyReadyTaskContext::~DirtyReadyTaskContext() noexcept {
  cancellation_registration_ = ComputeRunCancellationRegistration{};
  close_publication();
}

/** @copydoc DirtyReadyTaskContext::retained_memory_bytes */
std::uint64_t DirtyReadyTaskContext::retained_memory_bytes() const {
  RetainedMemoryEstimator estimate("DirtyReadyTaskContext");
  estimate.add_objects<DirtyReadyTaskContext>();
  estimate.add_shared_control_block();
  estimate.add_bytes(compute_plan_dynamic_retained_memory_bytes(compute_plan_));
  if (selection_.has_value()) {
    estimate.add_bytes(
        dirty_selection_dynamic_retained_memory_bytes(*selection_));
  }
  estimate.add_objects<int>(
      static_cast<std::uint64_t>(active_task_ids_.capacity()));
  estimate.add_objects<Device>(
      static_cast<std::uint64_t>(task_devices_.capacity()));
  estimate.add_objects<OperationExecutionConstraints>(
      static_cast<std::uint64_t>(task_constraints_.capacity()));
  for (const OperationExecutionConstraints& constraints : task_constraints_) {
    estimate.add_string_payload(constraints.exclusive_key);
  }
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(active_task_id_set_.bucket_count()));
  estimate.add_objects<decltype(active_task_id_set_)::value_type>(
      static_cast<std::uint64_t>(active_task_id_set_.size()));
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(active_task_id_set_.size()));
  estimate.add_objects<std::uint8_t>(
      static_cast<std::uint64_t>(task_states_.capacity()));
  estimate.add_objects<std::unique_ptr<DeferredValueWait>>(
      static_cast<std::uint64_t>(deferred_value_waits_.capacity()));
  for (const auto& wait : deferred_value_waits_) {
    if (wait) {
      estimate.add_objects<DeferredValueWait>();
    }
  }
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(active_task_id_set_.size()));
  if (dependency_state_) {
    estimate.add_objects<TaskDependencyState>();
    estimate.add_bytes(dependency_state_->dynamic_retained_memory_bytes());
  }
  estimate.add_bytes(run_task_retained_memory_bytes_);
  return estimate.bytes();
}

/** @copydoc DirtyReadyTaskContext::run_resource_demand */
CpuRunResourceDemand DirtyReadyTaskContext::run_resource_demand(
    std::uint64_t additional_shared_retained_memory_bytes) const {
  RetainedMemoryEstimator shared("dirty service phase");
  shared.add_bytes(retained_memory_bytes());
  shared.add_bytes(lease_.retained_memory_bytes());
  shared.add_bytes(additional_shared_retained_memory_bytes);
  const ReadyTaskResourceDemand callback_demand =
      owned_callback_resource_demand(static_cast<std::uint64_t>(
          sizeof(std::shared_ptr<DirtyReadyTaskContext>)));
  RetainedMemoryEstimator per_task_retained("dirty operation task demand");
  per_task_retained.add_bytes(callback_demand.retained_memory_bytes);
  per_task_retained.add_bytes(
      task_operation_resource_demand_.retained_memory_bytes);
  ReadyTaskResourceDemand combined_demand = callback_demand;
  combined_demand.retained_memory_bytes = per_task_retained.bytes();
  combined_demand.scratch_bytes = task_operation_resource_demand_.scratch_bytes;
  combined_demand.work_units = std::max(
      callback_demand.work_units, task_operation_resource_demand_.work_units);
  return CpuRunResourceDemand{shared.bytes(), combined_demand};
}

/** @copydoc DirtyReadyTaskContext::make_submissions */
std::vector<ReadyTaskSubmission> DirtyReadyTaskContext::make_submissions(
    const std::vector<int>& task_ids, bool initial_ready) {
  std::vector<ReadyTaskSubmission> submissions;
  submissions.reserve(task_ids.size());
  const std::shared_ptr<DirtyReadyTaskContext> self = shared_from_this();
  const ReadyTaskResourceDemand task_demand = run_resource_demand(0U).task;
  for (int task_id : task_ids) {
    if (task_id < 0 ||
        task_id >= static_cast<int>(compute_plan_.task_graph.tasks.size()) ||
        active_task_id_set_.count(task_id) == 0U) {
      throw std::invalid_argument(
          "Dirty ready submission names an inactive task.");
    }
    const PlannedTask& task = compute_plan_.task_graph.tasks.at(task_id);
    ComputeRunLease submission_lease = lease_;
    const ComputeRunTaskIdentity identity =
        submission_lease.task_identity(static_cast<uint64_t>(task_id));
    submissions.emplace_back(
        std::move(submission_lease), identity, task.node_id, initial_ready,
        [self](ComputeRunLease& lease,
               const ComputeRunTaskIdentity& accepted_identity,
               ExecutionTaskRuntime& task_runtime) {
          self->execute(lease, accepted_identity, task_runtime);
        },
        priority_, task_demand,
        task_devices_.at(static_cast<std::size_t>(task_id)),
        std::move(task_constraints_.at(static_cast<std::size_t>(task_id))));
  }
  return submissions;
}

/** @copydoc DirtyReadyTaskContext::execute */
void DirtyReadyTaskContext::execute(ComputeRunLease& lease,
                                    const ComputeRunTaskIdentity& identity,
                                    ExecutionTaskRuntime& task_runtime) {
  if (identity.run_id() != lease.descriptor().id() ||
      identity.run_id() != lease_.descriptor().id()) {
    throw std::invalid_argument(
        "Dirty ready task identity does not match its Run lease.");
  }
  const uint64_t local_value = identity.local_task_id().value();
  if (local_value > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("Dirty ready task id exceeds int range.");
  }
  const int task_id = static_cast<int>(local_value);
  if (task_id < 0 ||
      task_id >= static_cast<int>(compute_plan_.task_graph.tasks.size()) ||
      active_task_id_set_.count(task_id) == 0U) {
    throw std::invalid_argument(
        "Dirty ready task identity is not active in this Run phase.");
  }

  const PlannedTask& task = compute_plan_.task_graph.tasks.at(task_id);
  try {
    {
      std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
      std::uint8_t& state = task_states_.at(static_cast<std::size_t>(task_id));
      if (state != static_cast<std::uint8_t>(TaskState::Pending)) {
        throw std::logic_error("Dirty ready task entered more than once.");
      }
      state = static_cast<std::uint8_t>(TaskState::Executing);
    }
    if (lease.observe_cancellation().has_value()) {
      std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
      task_states_.at(static_cast<std::size_t>(task_id)) =
          static_cast<std::uint8_t>(TaskState::Completed);
      task_runtime.dec_tasks_to_complete();
      return;
    }
    run_task_(task_id);
    if (defer_pending_value(task, lease, task_runtime)) {
      task_runtime.dec_tasks_to_complete();
      return;
    }
    if (lease.observe_cancellation().has_value()) {
      std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
      task_states_.at(static_cast<std::size_t>(task_id)) =
          static_cast<std::uint8_t>(TaskState::Completed);
      task_runtime.dec_tasks_to_complete();
      return;
    }
    {
      std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
      if (!publication_closed_ && !lease.terminal_outcome().has_value()) {
        release_task_dependents(task, lease, task_runtime);
      }
      task_states_.at(static_cast<std::size_t>(task_id)) =
          static_cast<std::uint8_t>(TaskState::Completed);
    }
    task_runtime.dec_tasks_to_complete();
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    {
      std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
      task_states_.at(static_cast<std::size_t>(task_id)) =
          static_cast<std::uint8_t>(TaskState::Failed);
    }
    close_publication();
    try {
      task_runtime.log_event(ExecutionTraceAction::RethrowException,
                             task.node_id);
    } catch (...) {
    }
    try {
      (void)lease_.publish_failure(failure);
    } catch (...) {
    }
    std::rethrow_exception(failure);
  }
}

/** @copydoc DirtyReadyTaskContext::output_authority_for */
const PlannedOutputAuthority& DirtyReadyTaskContext::output_authority_for(
    const PlannedTask& task) const {
  const auto found = std::find_if(compute_plan_.planned_work.begin(),
                                  compute_plan_.planned_work.end(),
                                  [&task](const PlannedNodeWork& work) {
                                    return work.node_id == task.node_id;
                                  });
  if (found == compute_plan_.planned_work.end() ||
      !found->output_authority.has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Dirty task is missing frozen output authority.");
  }
  return *found->output_authority;
}

/** @copydoc DirtyReadyTaskContext::defer_pending_value */
bool DirtyReadyTaskContext::defer_pending_value(
    const PlannedTask& task, ComputeRunLease& lease,
    ExecutionTaskRuntime& task_runtime) {
  if (snapshot_task_output_ == nullptr) {
    return false;
  }
  std::optional<NodeOutput> output =
      snapshot_task_output_(task_output_context_, task.node_id);
  if (!output.has_value()) {
    return false;
  }
  if (task.kind == PlannedTaskKind::Tile && !output->has_image_value()) {
    return false;
  }
  validate_planned_output(*output, output_authority_for(task),
                          PlannedOutputReadiness::AllowPending);
  std::string expected_name;
  Value expected_value;
  for (const auto& [name, value] : output->named_values) {
    const ReadyFenceSnapshot observed = value.ready_fence().poll();
    if (observed.state() == ReadyFenceState::Pending) {
      expected_name = name;
      expected_value = value;
      break;
    }
    if (observed.state() != ReadyFenceState::Ready) {
      throw ReadyFenceAccessError(observed);
    }
  }
  if (!expected_value.valid()) {
    return false;
  }

  std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
  if (publication_closed_ || lease.terminal_outcome().has_value()) {
    return false;
  }
  const std::size_t task_index = static_cast<std::size_t>(task.task_id);
  DeferredValueWait* record = deferred_value_waits_.at(task_index).get();
  if (record == nullptr ||
      record->completion_outstanding.load(std::memory_order_acquire) ||
      record->registration.active()) {
    throw std::logic_error(
        "Dirty task attempted duplicate pending-Value registration.");
  }
  std::shared_ptr<ReadyFenceExecutor> executor =
      task_runtime.make_ready_fence_executor();
  const std::shared_ptr<DirtyReadyTaskContext> self = shared_from_this();
  ReadyFence::Callback callback =
      [self, task_id = task.task_id, expected_name, expected_value,
       record](ReadyFenceSnapshot snapshot) mutable {
        self->complete_deferred_value(task_id, std::move(expected_name),
                                      std::move(expected_value), record,
                                      std::move(snapshot));
      };
  record->runtime = &task_runtime;
  record->completion_outstanding.store(true, std::memory_order_release);
  task_states_.at(task_index) =
      static_cast<std::uint8_t>(TaskState::AwaitingValue);
  bool completion_counted = false;
  try {
    task_runtime.inc_tasks_to_complete(1);
    completion_counted = true;
    record->registration = expected_value.ready_fence().async_wait(
        std::move(executor), std::move(callback));
  } catch (...) {
    task_states_.at(task_index) = static_cast<std::uint8_t>(TaskState::Failed);
    record->completion_outstanding.store(false, std::memory_order_release);
    record->runtime = nullptr;
    if (completion_counted) {
      task_runtime.dec_tasks_to_complete();
    }
    throw;
  }
  return true;
}

/** @copydoc DirtyReadyTaskContext::release_task_dependents */
void DirtyReadyTaskContext::release_task_dependents(
    const PlannedTask& task, ComputeRunLease& lease,
    ExecutionTaskRuntime& task_runtime) {
  if (!release_dependents_) {
    return;
  }
  const std::vector<int> ready_ids =
      dependency_state_->release_dependents(task.task_id);
  std::vector<ReadyTaskSubmission> ready_submissions =
      make_submissions(ready_ids, false);
  auto* ready_runtime =
      dynamic_cast<ReadyTaskSubmissionRuntime*>(&task_runtime);
  if (ready_runtime == nullptr) {
    throw std::logic_error(
        "Dirty owned context requires a ready-submission runtime.");
  }
  for (ReadyTaskSubmission& submission : ready_submissions) {
    if (lease.observe_cancellation().has_value() ||
        lease.terminal_outcome().has_value()) {
      break;
    }
    ready_runtime->submit_ready_submission(std::move(submission));
  }
}

/** @copydoc DirtyReadyTaskContext::complete_deferred_value */
void DirtyReadyTaskContext::complete_deferred_value(
    int task_id, std::string expected_name, Value expected_value,
    DeferredValueWait* record, ReadyFenceSnapshot snapshot) {
  if (record == nullptr || record->runtime == nullptr ||
      !record->completion_outstanding.exchange(false,
                                               std::memory_order_acq_rel)) {
    return;
  }
  ExecutionTaskRuntime& task_runtime = *record->runtime;
  std::exception_ptr failure;
  bool deferred_again = false;
  const PlannedTask& task =
      compute_plan_.task_graph.tasks.at(static_cast<std::size_t>(task_id));
  try {
    bool suppress_completion = false;
    {
      std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
      std::uint8_t& state = task_states_.at(static_cast<std::size_t>(task_id));
      if (state != static_cast<std::uint8_t>(TaskState::AwaitingValue)) {
        throw std::logic_error(
            "Dirty pending-Value continuation entered more than once.");
      }
      state = static_cast<std::uint8_t>(TaskState::CompletingValue);
      if (publication_closed_ || lease_.terminal_outcome().has_value()) {
        state = static_cast<std::uint8_t>(TaskState::Completed);
        suppress_completion = true;
      }
    }
    if (!suppress_completion) {
      if (snapshot.state() != ReadyFenceState::Ready) {
        throw ReadyFenceAccessError(std::move(snapshot));
      }
      std::optional<NodeOutput> output =
          snapshot_task_output_ == nullptr
              ? std::nullopt
              : snapshot_task_output_(task_output_context_, task.node_id);
      if (!output.has_value()) {
        throw GraphError(GraphErrc::ComputeError,
                         "Dirty continuation lost its staged output.");
      }
      const auto current = output->named_values.find(expected_name);
      if (current == output->named_values.end() ||
          !same_value_publication_identity(current->second, expected_value)) {
        throw GraphError(
            GraphErrc::ComputeError,
            "Dirty continuation observed a replaced staged Value.");
      }
      validate_planned_output(*output, output_authority_for(task),
                              PlannedOutputReadiness::AllowPending);
      std::string next_name;
      Value next_value;
      for (const auto& [name, value] : output->named_values) {
        const ReadyFenceSnapshot observed = value.ready_fence().poll();
        if (observed.state() == ReadyFenceState::Pending) {
          next_name = name;
          next_value = value;
          break;
        }
        if (observed.state() != ReadyFenceState::Ready) {
          throw ReadyFenceAccessError(observed);
        }
      }
      if (next_value.valid()) {
        std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
        if (publication_closed_ || lease_.terminal_outcome().has_value()) {
          task_states_.at(static_cast<std::size_t>(task_id)) =
              static_cast<std::uint8_t>(TaskState::Completed);
          suppress_completion = true;
        } else {
          std::shared_ptr<ReadyFenceExecutor> executor =
              task_runtime.make_ready_fence_executor();
          const std::shared_ptr<DirtyReadyTaskContext> self =
              shared_from_this();
          ReadyFence::Callback callback =
              [self, task_id, next_name, next_value,
               record](ReadyFenceSnapshot next_snapshot) mutable {
                self->complete_deferred_value(task_id, std::move(next_name),
                                              std::move(next_value), record,
                                              std::move(next_snapshot));
              };
          record->completion_outstanding.store(true, std::memory_order_release);
          task_states_.at(static_cast<std::size_t>(task_id)) =
              static_cast<std::uint8_t>(TaskState::AwaitingValue);
          bool completion_counted = false;
          try {
            task_runtime.inc_tasks_to_complete(1);
            completion_counted = true;
            record->registration = next_value.ready_fence().async_wait(
                std::move(executor), std::move(callback));
            deferred_again = true;
          } catch (...) {
            task_states_.at(static_cast<std::size_t>(task_id)) =
                static_cast<std::uint8_t>(TaskState::Failed);
            record->completion_outstanding.store(false,
                                                 std::memory_order_release);
            if (completion_counted) {
              task_runtime.dec_tasks_to_complete();
            }
            throw;
          }
        }
      }
      if (!suppress_completion && !deferred_again) {
        validate_planned_output(*output, output_authority_for(task),
                                PlannedOutputReadiness::RequireReady);
      }
      {
        std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
        (void)lease_.observe_cancellation();
        if (!suppress_completion && !deferred_again && !publication_closed_ &&
            !lease_.terminal_outcome().has_value()) {
          release_task_dependents(task, lease_, task_runtime);
        }
        if (!deferred_again) {
          task_states_.at(static_cast<std::size_t>(task_id)) =
              static_cast<std::uint8_t>(TaskState::Completed);
        }
      }
    }
  } catch (...) {
    failure = std::current_exception();
    {
      std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
      task_states_.at(static_cast<std::size_t>(task_id)) =
          static_cast<std::uint8_t>(TaskState::Failed);
    }
    close_publication();
    try {
      task_runtime.log_event(ExecutionTraceAction::RethrowException,
                             task.node_id);
    } catch (...) {
    }
  }
  try {
    task_runtime.dec_tasks_to_complete();
  } catch (...) {
    if (!failure) {
      failure = std::current_exception();
    }
  }
  if (!deferred_again) {
    record->runtime = nullptr;
  }
  if (failure) {
    try {
      (void)lease_.publish_failure(failure);
    } catch (...) {
    }
    std::rethrow_exception(failure);
  }
}

/** @copydoc DirtyReadyTaskContext::close_publication */
void DirtyReadyTaskContext::close_publication() noexcept {
  std::vector<std::pair<std::size_t, ReadyFenceWaitRegistration>> retired_waits;
  try {
    {
      std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
      if (publication_closed_) {
        return;
      }
      publication_closed_ = true;
      retired_waits.reserve(deferred_value_waits_.size());
      for (std::size_t task_index = 0U;
           task_index < deferred_value_waits_.size(); ++task_index) {
        const std::unique_ptr<DeferredValueWait>& record =
            deferred_value_waits_[task_index];
        if (record != nullptr) {
          retired_waits.emplace_back(task_index,
                                     std::move(record->registration));
        }
      }
    }

    for (auto& retired : retired_waits) {
      const std::size_t task_index = retired.first;
      DeferredValueWait* const record =
          deferred_value_waits_.at(task_index).get();
      if (retired.second.cancel() && record->completion_outstanding.exchange(
                                         false, std::memory_order_acq_rel)) {
        {
          std::lock_guard<std::recursive_mutex> lock(publication_mutex_);
          if (task_index < task_states_.size() &&
              task_states_[task_index] ==
                  static_cast<std::uint8_t>(TaskState::AwaitingValue)) {
            task_states_[task_index] =
                static_cast<std::uint8_t>(TaskState::Completed);
          }
        }
        record->runtime = nullptr;
      }
    }
  } catch (...) {
    std::terminate();
  }
}

void remember_dirty_snapshot(GraphModel& graph,
                             const DirtyRegionSnapshot& snapshot) {
  graph.last_dirty_region_snapshot = snapshot;
  graph.recent_dirty_region_snapshots.push_back(snapshot);
  if (graph.recent_dirty_region_snapshots.size() > 16) {
    graph.recent_dirty_region_snapshots.erase(
        graph.recent_dirty_region_snapshots.begin());
  }
}

void remember_compute_plan(GraphModel& graph, const ComputePlan& compute_plan,
                           const DirtyTaskSelectionOverlay* selection) {
  graph.last_compute_plan = compute_plan;
  graph.last_compute_plan_summary =
      summarize_compute_plan(graph, compute_plan, selection);
  graph.recent_compute_plan_summaries.push_back(
      *graph.last_compute_plan_summary);
  if (graph.recent_compute_plan_summaries.size() > 16) {
    graph.recent_compute_plan_summaries.erase(
        graph.recent_compute_plan_summaries.begin());
  }
}

ComputePlan prune_node_cache_task_graph(
    GraphModel& graph, const ComputeRequest& request,
    const std::vector<int>& execution_order,
    const std::vector<Device>& available_devices) {
  NodeCacheTaskGraphPruner node_cache_pruner;
  const std::shared_ptr<const FullTaskGraph> full_graph =
      get_or_expand_full_task_graph(graph, request.intent, available_devices);
  return node_cache_pruner.prune(*full_graph, request, execution_order, graph);
}

ComputePlan prune_dirty_snapshot_task_graph(const ComputePlan& node_cache_plan,
                                            const DirtyRegionSnapshot& snapshot,
                                            const GraphModel& graph) {
  DirtySnapshotTaskGraphPruner dirty_snapshot_pruner;
  return dirty_snapshot_pruner.prune(node_cache_plan, snapshot, graph);
}

std::vector<int> planned_nodes_for_task_ids(const ComputePlan& compute_plan,
                                            const std::vector<int>& task_ids) {
  std::vector<int> node_ids;
  std::unordered_set<int> selected_node_ids;
  node_ids.reserve(task_ids.size());
  selected_node_ids.reserve(task_ids.size());
  for (int task_id : task_ids) {
    if (task_id < 0 ||
        task_id >= static_cast<int>(compute_plan.task_graph.tasks.size())) {
      throw std::out_of_range("Dirty phase task id is outside ComputePlan.");
    }
    const int node_id = compute_plan.task_graph.tasks.at(task_id).node_id;
    if (selected_node_ids.insert(node_id).second) {
      node_ids.push_back(node_id);
    }
  }
  return node_ids;
}

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
    (void)domain;
    const NodeOutput* output = ComputeCachePolicy::reusable_output(*source);
    if (!output) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Dirty source boundary output is not ready for node " +
                           std::to_string(source_node_id) + ".");
    }
  }
}

bool is_dirty_source_node(const DirtyRegionSnapshot& snapshot, int node_id) {
  return std::find(snapshot.dirty_source_nodes.begin(),
                   snapshot.dirty_source_nodes.end(),
                   node_id) != snapshot.dirty_source_nodes.end();
}

void log_dirty_node_execution(GraphRuntime* runtime, int node_id,
                              bool dirty_source) {
  if (!runtime) {
    return;
  }
  runtime->log_event(GraphRuntime::ExecutionEvent::EXECUTE, node_id);
  runtime->log_event(
      dirty_source
          ? GraphRuntime::ExecutionEvent::EXECUTE_DIRTY_SOURCE
          : GraphRuntime::ExecutionEvent::EXECUTE_DIRTY_DOWNSTREAM_NODE,
      node_id);
}

bool should_skip_stale_dirty_source(GraphRuntime* runtime, int node_id,
                                    uint64_t committed_generation,
                                    uint64_t dirty_generation) {
  if (committed_generation <= dirty_generation) {
    return false;
  }
  if (runtime) {
    runtime->log_event(GraphRuntime::ExecutionEvent::SKIP_STALE_GENERATION,
                       node_id);
  }
  return true;
}

/**
 * @brief Resolves the output format for a dirty-domain staging buffer.
 * @param preferred Existing staged output preferred when it has a valid image.
 * @param image_inputs Destination-indexed image inputs, including null slots.
 * @param fallback Optional committed output used after staged/input candidates.
 * @return Channel count and data type from the first usable candidate, or the
 * single-channel FLOAT32 default.
 * @throws Nothing.
 * @note Disconnected input placeholders are skipped only for format inference;
 * their slot identity remains intact for operation execution.
 */
std::pair<int, DataType> infer_output_spec(
    const std::optional<NodeOutput>& preferred,
    const std::vector<const NodeOutput*>& image_inputs,
    const std::optional<NodeOutput>* fallback) {
  const auto value_spec =
      [](const NodeOutput& output) -> std::optional<std::pair<int, DataType>> {
    if (!output.has_image_value() ||
        !output.image_value().image_facet().has_value()) {
      return std::nullopt;
    }
    const Value& value = output.image_value();
    const DenseTensorDescriptor& descriptor = value.dense_tensor_descriptor();
    const ImageFacet& facet = *value.image_facet();
    const std::size_t channels = facet.channel_axis.has_value()
                                     ? descriptor.shape[*facet.channel_axis]
                                     : 1U;
    if (channels > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      throw std::invalid_argument(
          "Dirty output channel count exceeds int compatibility.");
    }
    const std::uint32_t bits = descriptor.storage_encoding.bit_width;
    DataType type;
    switch (descriptor.element_semantics) {
      case ElementSemantics::UnsignedInteger:
        if (bits == 8U) {
          type = DataType::UINT8;
        } else if (bits == 16U) {
          type = DataType::UINT16;
        } else {
          throw std::invalid_argument(
              "Dirty output unsigned element width is unsupported.");
        }
        break;
      case ElementSemantics::SignedInteger:
        if (bits == 8U) {
          type = DataType::INT8;
        } else if (bits == 16U) {
          type = DataType::INT16;
        } else {
          throw std::invalid_argument(
              "Dirty output signed element width is unsupported.");
        }
        break;
      case ElementSemantics::FloatingPoint:
        if (bits == 32U) {
          type = DataType::FLOAT32;
        } else if (bits == 64U) {
          type = DataType::FLOAT64;
        } else {
          throw std::invalid_argument(
              "Dirty output floating element width is unsupported.");
        }
        break;
    }
    return std::pair<int, DataType>{static_cast<int>(channels), type};
  };
  if (preferred) {
    if (const auto spec = value_spec(*preferred)) {
      return *spec;
    }
  }
  for (const auto* input : image_inputs) {
    if (!input) {
      continue;
    }
    if (const auto spec = value_spec(*input)) {
      return *spec;
    }
  }
  if (fallback && *fallback) {
    if (const auto spec = value_spec(**fallback)) {
      return *spec;
    }
  }
  return {1, DataType::FLOAT32};
}

/** @copydoc validate_dirty_region_operation_routes */
void validate_dirty_region_operation_routes(
    const GraphModel& graph,
    const DirtyRegionOperationRouteSnapshot& route_snapshot,
    const ComputePlan& compute_plan, const DirtyTaskSelectionOverlay& selection,
    const ComputeRequest& request) {
  if (route_snapshot.node_routes.empty()) {
    return;
  }
  if (selection.active_task_ids.empty()) {
    return;
  }
  if (route_snapshot.intent != request.intent ||
      route_snapshot.intent != compute_plan.intent) {
    throw GraphError(
        GraphErrc::NoOperation,
        "Dirty Region operation route intent changed before task population.");
  }
  if (route_snapshot.available_devices != compute_plan.available_devices) {
    throw GraphError(
        GraphErrc::NoOperation,
        "Dirty Region operation device inventory changed before task "
        "population.");
  }

  std::vector<int> active_node_ids;
  active_node_ids.reserve(selection.active_task_ids.size());
  std::unordered_set<int> active_nodes;
  active_nodes.reserve(selection.active_task_ids.size());
  for (int task_id : selection.active_task_ids) {
    if (task_id < 0 || static_cast<std::size_t>(task_id) >=
                           compute_plan.task_graph.tasks.size()) {
      throw GraphError(GraphErrc::ComputeError,
                       "Dirty task selection contains an invalid task id.");
    }
    const int node_id =
        compute_plan.task_graph.tasks.at(static_cast<std::size_t>(task_id))
            .node_id;
    if (active_nodes.insert(node_id).second) {
      active_node_ids.push_back(node_id);
    }
  }

  for (int node_id : active_node_ids) {
    const auto frozen = route_snapshot.node_routes.find(node_id);
    const Node* node = graph.find_node(node_id);
    const auto planned = std::find_if(compute_plan.planned_work.begin(),
                                      compute_plan.planned_work.end(),
                                      [node_id](const PlannedNodeWork& work) {
                                        return work.node_id == node_id;
                                      });
    const bool route_matches =
        frozen != route_snapshot.node_routes.end() && node != nullptr &&
        frozen->second.operation_key == make_key(node->type, node->subtype) &&
        planned != compute_plan.planned_work.end() &&
        planned->operation_route.has_value() &&
        planned_operation_routes_equal(frozen->second.route,
                                       *planned->operation_route);
    if (!route_matches) {
      throw GraphError(
          GraphErrc::NoOperation,
          "Dirty Region operation route changed before task population at "
          "node " +
              std::to_string(node_id) + ".");
    }
  }
}

/**
 * @brief Applies selected HP image work to derived ROI and Region metadata.
 * @param entries HP plan entries retained by the prepared request.
 * @param selection Active task-selection overlay.
 * @return Nothing.
 * @throws std::invalid_argument or std::overflow_error when the retained data
 * window cannot represent the selected storage ROI.
 * @throws std::bad_alloc when exact ImageRect Region storage cannot allocate.
 * @note TensorSlice plans have no represented_hp_roi and retain their original
 *       authoritative Region.
 */
void apply_planned_work_rois(std::unordered_map<int, HpPlanEntry>& entries,
                             const DirtyTaskSelectionOverlay& selection) {
  for (const auto& [node_id, node_selection] : selection.node_selections) {
    auto entry_it = entries.find(node_id);
    if (entry_it == entries.end()) {
      continue;
    }
    if (!is_rect_empty(node_selection.represented_hp_roi)) {
      entry_it->second.roi_hp = clip_rect(node_selection.represented_hp_roi,
                                          entry_it->second.hp_size);
      entry_it->second.region_hp =
          region_image_adapter::from_storage_pixel_rect(
              entry_it->second.roi_hp, entry_it->second.hp_data_window);
    }
  }
}

/**
 * @brief Applies selected RT image work to HP Region and RT ROI projections.
 * @param entries RT plan entries retained by the prepared request.
 * @param selection Active task-selection overlay.
 * @return Nothing.
 * @throws std::invalid_argument or std::overflow_error when the retained data
 * window cannot represent the selected storage ROI.
 * @throws std::bad_alloc when exact ImageRect Region storage cannot allocate.
 * @note RT selection remains image-only; no TensorSlice projection occurs.
 */
void apply_planned_work_rois(std::unordered_map<int, RtPlanEntry>& entries,
                             const DirtyTaskSelectionOverlay& selection) {
  for (const auto& [node_id, node_selection] : selection.node_selections) {
    auto entry_it = entries.find(node_id);
    if (entry_it == entries.end()) {
      continue;
    }
    if (!is_rect_empty(node_selection.represented_hp_roi)) {
      entry_it->second.roi_hp = clip_rect(node_selection.represented_hp_roi,
                                          entry_it->second.hp_size);
      entry_it->second.region_hp =
          region_image_adapter::from_storage_pixel_rect(
              entry_it->second.roi_hp, entry_it->second.hp_data_window);
    }
    if (!is_rect_empty(node_selection.execution_roi)) {
      entry_it->second.roi_rt =
          clip_rect(node_selection.execution_roi, entry_it->second.rt_size);
    }
  }
}

}  // namespace ps::compute
