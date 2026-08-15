#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "compute/execution/execution_service_internal.hpp"

/**
 * @file execution_service_state.cpp
 * @brief Owns retained-envelope calculations and value-state lifetimes.
 */

namespace ps::compute {

using namespace execution_service_detail;  // NOLINT(build/namespaces)

/**
 * @brief Calculates the mandatory structural bytes for one submission.
 * @param graph_identity Stable copied metadata string carried by every task.
 * @return Queue entry, shared owner, ready-store handle, and string bytes.
 * @throws GraphError when checked structural arithmetic overflows.
 * @note The returned envelope excludes adapter-declared capture bytes so they
 * can be added exactly once in the relevant lifecycle dimension. Copied string
 * storage is charged by actual capacity plus its null terminator.
 */
std::uint64_t ExecutionService::service_submission_envelope_bytes(
    const std::string& graph_identity) {
  RetainedMemoryEstimator estimate("ExecutionService submission envelope");
  estimate.add_objects<QueueEntry>();
  estimate.add_objects<std::shared_ptr<QueueEntry>>();
  estimate.add_objects<void*>(2U);
  estimate.add_shared_control_block();
  estimate.add_string_payload(graph_identity);
  return estimate.bytes();
}

/** @copydoc ExecutionService::calculate_policy_service_cost */
std::uint64_t ExecutionService::calculate_policy_service_cost(
    ReadyTaskResourceDemand demand, std::uint64_t complete_ready_bytes) {
  if (demand.work_units == 0U) {
    throw std::invalid_argument(
        "ExecutionService policy work units must be positive.");
  }
  if (complete_ready_bytes == 0U) {
    throw GraphError(GraphErrc::ComputeError,
                     "ExecutionService policy ready bytes must be positive.");
  }

  const std::uint64_t quotient = complete_ready_bytes / kPolicyReadyByteQuantum;
  const std::uint64_t remainder =
      complete_ready_bytes % kPolicyReadyByteQuantum;
  if (remainder != 0U &&
      quotient == std::numeric_limits<std::uint64_t>::max()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ExecutionService policy byte quantum overflow.");
  }
  const std::uint64_t byte_quanta = quotient + (remainder != 0U ? 1U : 0U);
  if (byte_quanta >
      std::numeric_limits<std::uint64_t>::max() - demand.work_units) {
    throw GraphError(GraphErrc::ComputeError,
                     "ExecutionService policy service cost overflow.");
  }
  return demand.work_units + byte_quanta;
}

/**
 * @brief Builds one checked service-plus-adapter CPU Run estimate.
 * @param configured_workers Frozen service worker count.
 * @param graph_identity Copied metadata identity shared by logical tasks.
 * @param total_task_count Positive complete task count.
 * @param maximum_parallelism Optional positive Run callback-concurrency cap.
 * @param demand Shared once-per-Run and uniform per-task declarations.
 * @return Complete root vector and reusable child-grant envelopes.
 * @throws std::invalid_argument for a nonpositive task count.
 * @throws GraphError when any addition or multiplication overflows.
 * @note Retained and scratch task bytes scale with maximum callback
 * concurrency: the minimum of fixed workers, logical tasks, and the optional
 * Run cap. Ready entries/bytes scale with every logical task so dependency
 * release cannot exceed the admitted reservation.
 */
ExecutionService::CpuRunAdmissionEstimate
ExecutionService::calculate_cpu_run_admission(
    unsigned int configured_workers, const std::string& graph_identity,
    int total_task_count, std::optional<std::uint32_t> maximum_parallelism,
    CpuRunResourceDemand demand) {
  if (total_task_count <= 0) {
    throw std::invalid_argument(
        "ExecutionService requires a positive total task count.");
  }

  const std::uint64_t service_task_bytes =
      service_submission_envelope_bytes(graph_identity);
  RetainedMemoryEstimator ready_estimate(
      "ExecutionService per-task ready envelope");
  ready_estimate.add_bytes(service_task_bytes);
  ready_estimate.add_bytes(demand.task.ready_bytes);
  const std::uint64_t ready_bytes_per_task = ready_estimate.bytes();

  RetainedMemoryEstimator execution_estimate(
      "ExecutionService per-task execution envelope");
  execution_estimate.add_bytes(service_task_bytes);
  execution_estimate.add_bytes(demand.task.retained_memory_bytes);
  const std::uint64_t execution_bytes_per_task = execution_estimate.bytes();

  std::string policy_graph_identity(graph_identity);
  std::string policy_graph_key(graph_identity);

  RetainedMemoryEstimator shared_estimate(
      "ExecutionService shared Run envelope");
  shared_estimate.add_bytes(
      service_run_envelope_bytes(policy_graph_identity, policy_graph_key));
  shared_estimate.add_bytes(demand.shared_retained_memory_bytes);

  const std::uint64_t logical_task_count =
      static_cast<std::uint64_t>(total_task_count);
  std::uint64_t concurrent_task_count = std::min(
      static_cast<std::uint64_t>(configured_workers), logical_task_count);
  if (maximum_parallelism.has_value()) {
    concurrent_task_count =
        std::min(concurrent_task_count,
                 static_cast<std::uint64_t>(*maximum_parallelism));
  }

  const std::optional<ResourceVector> execution_resources =
      checked_multiply_resources(
          ResourceVector{0U, execution_bytes_per_task,
                         demand.task.scratch_bytes, 0U, 0U},
          concurrent_task_count);
  const std::optional<ResourceVector> ready_resources =
      checked_multiply_resources(
          ResourceVector{0U, 0U, 0U, 1U, ready_bytes_per_task},
          logical_task_count);
  if (!execution_resources.has_value() || !ready_resources.has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ExecutionService Run resource aggregation overflow.");
  }

  const ResourceVector shared_resources{concurrent_task_count,
                                        shared_estimate.bytes(), 0U, 0U, 0U};
  const std::optional<ResourceVector> with_execution =
      checked_add_resources(shared_resources, *execution_resources);
  const std::optional<ResourceVector> complete =
      with_execution.has_value()
          ? checked_add_resources(*with_execution, *ready_resources)
          : std::nullopt;
  if (!complete.has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ExecutionService Run resource aggregation overflow.");
  }
  return CpuRunAdmissionEstimate{
      *complete,
      ready_bytes_per_task,
      execution_bytes_per_task,
      std::move(policy_graph_identity),
      std::move(policy_graph_key),
  };
}

/** @copydoc PreparedExecutionRun::PreparedExecutionRun */
PreparedExecutionRun::PreparedExecutionRun() noexcept = default;

/** @copydoc PreparedExecutionRun::PreparedExecutionRun */
PreparedExecutionRun::PreparedExecutionRun(
    std::unique_ptr<PreparedExecutionRunState> state) noexcept
    : state_(std::move(state)) {}

/** @copydoc PreparedExecutionRun::PreparedExecutionRun */
PreparedExecutionRun::PreparedExecutionRun(
    PreparedExecutionRun&& other) noexcept = default;  // NOLINT

/** @copydoc PreparedExecutionRun::operator= */
PreparedExecutionRun& PreparedExecutionRun::operator=(
    PreparedExecutionRun&& other) noexcept {
  if (this != &other) {
    if (state_) {
      std::terminate();
    }
    state_ = std::move(other.state_);
  }
  return *this;
}

/** @copydoc PreparedExecutionRun::~PreparedExecutionRun */
PreparedExecutionRun::~PreparedExecutionRun() noexcept = default;

/** @copydoc PreparedExecutionSharedReservation::
 * PreparedExecutionSharedReservation */
PreparedExecutionSharedReservation::
    PreparedExecutionSharedReservation() noexcept = default;

/** @copydoc PreparedExecutionSharedReservation::
 * PreparedExecutionSharedReservation */
PreparedExecutionSharedReservation::PreparedExecutionSharedReservation(
    std::unique_ptr<PreparedExecutionSharedReservationState> state) noexcept
    : state_(std::move(state)) {}

/** @copydoc PreparedExecutionSharedReservation::
 * PreparedExecutionSharedReservation */
PreparedExecutionSharedReservation::PreparedExecutionSharedReservation(
    PreparedExecutionSharedReservation&& other) noexcept = default;  // NOLINT

/** @copydoc PreparedExecutionSharedReservation::operator= */
PreparedExecutionSharedReservation&
PreparedExecutionSharedReservation::operator=(
    PreparedExecutionSharedReservation&& other) noexcept {
  if (this != &other) {
    if (state_) {
      std::terminate();
    }
    state_ = std::move(other.state_);
  }
  return *this;
}

/** @copydoc PreparedExecutionSharedReservation::
 * ~PreparedExecutionSharedReservation */
PreparedExecutionSharedReservation::
    ~PreparedExecutionSharedReservation() noexcept = default;

/** @copydoc ExecutionService::service_run_envelope_bytes */
std::uint64_t ExecutionService::service_run_envelope_bytes(
    const std::string& graph_identity, const std::string& graph_key) {
  RetainedMemoryEstimator estimate("ExecutionService Run envelope");
  estimate.add_objects<RunState>();
  estimate.add_shared_control_block();
  estimate.add_bytes(BoundedReadyStore::run_policy_envelope_bytes());
  estimate.add_string_payload(graph_identity);
  estimate.add_string_payload(graph_key);
  estimate.add_bytes(ResourceLedger::reservation_state_retained_memory_bytes());
  estimate.add_bytes(
      ComputeRunLease::cancellation_notification_retained_memory_bytes(
          static_cast<std::uint64_t>(sizeof(ExecutionService*)) +
          static_cast<std::uint64_t>(sizeof(std::weak_ptr<RunState>))));
  return estimate.bytes();
}
/** @copydoc OperationExecutionLease::OperationExecutionLease */
OperationExecutionLease::OperationExecutionLease() noexcept = default;

/** @copydoc OperationExecutionLease::OperationExecutionLease */
OperationExecutionLease::OperationExecutionLease(
    std::unique_ptr<OperationExecutionLeaseState> state) noexcept
    : state_(std::move(state)) {}

/** @copydoc OperationExecutionLease::OperationExecutionLease */
OperationExecutionLease::OperationExecutionLease(
    OperationExecutionLease&& other) noexcept = default;  // NOLINT

/** @copydoc OperationExecutionLease::operator= */
OperationExecutionLease& OperationExecutionLease::operator=(
    OperationExecutionLease&& other) noexcept {
  if (this != &other) {
    if (state_) {
      std::terminate();
    }
    state_ = std::move(other.state_);
  }
  return *this;
}

/** @copydoc OperationExecutionLease::~OperationExecutionLease */
OperationExecutionLease::~OperationExecutionLease() noexcept = default;

/** @brief Current service-worker Run context, null outside callbacks. */
thread_local ExecutionService::RunState* ExecutionService::tls_run_state_ =
    nullptr;

/** @brief Current worker-owned started QueueEntry, or null outside callback. */
thread_local ExecutionService::QueueEntry* ExecutionService::tls_queue_entry_ =
    nullptr;

/** @brief Lazy fence/original-retirement rendezvous for current callback. */
thread_local std::shared_ptr<ExecutionService::FenceContinuationGate>
    ExecutionService::tls_fence_continuation_gate_;

/** @brief Exact service owning the entered callback, or null outside one. */
thread_local ExecutionService* ExecutionService::tls_service_ = nullptr;

/** @brief Current service worker id, or -1 outside callbacks. */
thread_local int ExecutionService::tls_worker_id_ = -1;

#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
namespace testing {

/** @copydoc arm_reserved_start_rollback_probe_for_testing */
void arm_reserved_start_rollback_probe_for_testing() noexcept {
  ReservedStartProbeState& state = reserved_start_probe_state();
  state.armed.store(false, std::memory_order_release);
  state.calls.store(0U, std::memory_order_relaxed);
  for (ReservedStartProbeAttempt& attempt : state.attempts) {
    attempt.candidate_id.store(0U, std::memory_order_relaxed);
    attempt.entry_version.store(0U, std::memory_order_relaxed);
    attempt.route_generation.store(0U, std::memory_order_relaxed);
    attempt.cpu_slots.store(0U, std::memory_order_relaxed);
    attempt.retained_memory_bytes.store(0U, std::memory_order_relaxed);
    attempt.scratch_bytes.store(0U, std::memory_order_relaxed);
    attempt.ready_entries.store(0U, std::memory_order_relaxed);
    attempt.ready_bytes.store(0U, std::memory_order_relaxed);
  }
  state.armed.store(true, std::memory_order_release);
}

/** @copydoc reserved_start_rollback_probe_snapshot_for_testing */
ReservedStartRollbackProbeSnapshot
reserved_start_rollback_probe_snapshot_for_testing() noexcept {
  ReservedStartProbeState& state = reserved_start_probe_state();
  ReservedStartRollbackProbeSnapshot snapshot;
  snapshot.calls = state.calls.load(std::memory_order_acquire);
  for (std::size_t index = 0U; index < 2U; ++index) {
    const ReservedStartProbeAttempt& attempt = state.attempts[index];
    const std::uint64_t ready_bytes =
        attempt.ready_bytes.load(std::memory_order_acquire);
    snapshot.candidate_ids[index] =
        attempt.candidate_id.load(std::memory_order_relaxed);
    snapshot.entry_versions[index] =
        attempt.entry_version.load(std::memory_order_relaxed);
    snapshot.route_generations[index] =
        attempt.route_generation.load(std::memory_order_relaxed);
    snapshot.resources[index] = ResourceVector{
        attempt.cpu_slots.load(std::memory_order_relaxed),
        attempt.retained_memory_bytes.load(std::memory_order_relaxed),
        attempt.scratch_bytes.load(std::memory_order_relaxed),
        attempt.ready_entries.load(std::memory_order_relaxed),
        ready_bytes,
    };
  }
  return snapshot;
}

/** @copydoc disarm_reserved_start_rollback_probe_for_testing */
void disarm_reserved_start_rollback_probe_for_testing() noexcept {
  reserved_start_probe_state().armed.store(false, std::memory_order_release);
}

/** @copydoc arm_route_commit_failure_probe_for_testing */
void arm_route_commit_failure_probe_for_testing() noexcept {
  RouteCommitFailureProbeState& state = route_commit_failure_probe_state();
  state.armed.store(false, std::memory_order_release);
  state.triggered.store(false, std::memory_order_relaxed);
  state.armed.store(true, std::memory_order_release);
}

/** @copydoc route_commit_failure_probe_triggered_for_testing */
bool route_commit_failure_probe_triggered_for_testing() noexcept {
  return route_commit_failure_probe_state().triggered.load(
      std::memory_order_acquire);
}

/** @copydoc disarm_route_commit_failure_probe_for_testing */
void disarm_route_commit_failure_probe_for_testing() noexcept {
  route_commit_failure_probe_state().armed.store(false,
                                                 std::memory_order_release);
}

/** @copydoc set_service_start_arbitration_observer_for_testing */
void set_service_start_arbitration_observer_for_testing(
    ServiceStartArbitrationObserver observer, void* context) noexcept {
  ServiceStartArbitrationProbeState& state =
      service_start_arbitration_probe_state();
  state.context.store(context, std::memory_order_relaxed);
  state.observer.store(observer, std::memory_order_release);
}

/** @copydoc clear_service_start_arbitration_observer_for_testing */
void clear_service_start_arbitration_observer_for_testing() noexcept {
  ServiceStartArbitrationProbeState& state =
      service_start_arbitration_probe_state();
  state.observer.store(nullptr, std::memory_order_release);
  state.context.store(nullptr, std::memory_order_relaxed);
}

/** @copydoc estimate_direct_operation_resources_for_testing */
ResourceVector estimate_direct_operation_resources_for_testing(
    const OperationExecutionConstraints& constraints,
    ReadyTaskResourceDemand demand) {
  const OperationExecutionConstraints retained_constraints(constraints);
  return direct_operation_execution_resources(retained_constraints, demand);
}

/** @copydoc direct_operation_fixed_retained_memory_bytes_for_testing */
std::uint64_t direct_operation_fixed_retained_memory_bytes_for_testing() {
  return direct_operation_fixed_retained_memory_bytes();
}

/** @copydoc set_operation_admission_wait_observer_for_testing */
void set_operation_admission_wait_observer_for_testing(
    OperationAdmissionWaitObserver observer, void* context) noexcept {
  OperationAdmissionWaitProbeState& state =
      operation_admission_wait_probe_state();
  state.context.store(context, std::memory_order_relaxed);
  state.observer.store(observer, std::memory_order_release);
}

/** @copydoc clear_operation_admission_wait_observer_for_testing */
void clear_operation_admission_wait_observer_for_testing() noexcept {
  OperationAdmissionWaitProbeState& state =
      operation_admission_wait_probe_state();
  state.observer.store(nullptr, std::memory_order_release);
  state.context.store(nullptr, std::memory_order_relaxed);
}

/** @copydoc set_retained_operation_string_charge_observer_for_testing */
void set_retained_operation_string_charge_observer_for_testing(
    RetainedOperationStringChargeObserver observer, void* context) noexcept {
  RetainedOperationStringChargeProbeState& state =
      retained_operation_string_charge_probe_state();
  state.context.store(context, std::memory_order_relaxed);
  state.observer.store(observer, std::memory_order_release);
}

/** @copydoc clear_retained_operation_string_charge_observer_for_testing */
void clear_retained_operation_string_charge_observer_for_testing() noexcept {
  RetainedOperationStringChargeProbeState& state =
      retained_operation_string_charge_probe_state();
  state.observer.store(nullptr, std::memory_order_release);
  state.context.store(nullptr, std::memory_order_relaxed);
}

/** @copydoc notify_retained_operation_string_charge_for_testing */
void notify_retained_operation_string_charge_for_testing(
    RetainedOperationStringOwner owner, const std::string& value,
    std::uint64_t before_bytes, std::uint64_t after_bytes) noexcept {
  RetainedOperationStringChargeProbeState& state =
      retained_operation_string_charge_probe_state();
  const RetainedOperationStringChargeObserver observer =
      state.observer.load(std::memory_order_acquire);
  if (observer != nullptr) {
    observer(state.context.load(std::memory_order_relaxed), owner,
             static_cast<std::uint64_t>(value.capacity()), before_bytes,
             after_bytes);
  }
}

}  // namespace testing
#endif

}  // namespace ps::compute
