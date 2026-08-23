#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "compute/execution/execution_service_internal.hpp"

/**
 * @file execution_service_admission.cpp
 * @brief Owns Run estimation, preparation, and direct-operation admission.
 */

namespace ps::compute {

using namespace execution_service_detail;  // NOLINT(build/namespaces)

/** @copydoc ExecutionService::lifecycle_snapshot */
ExecutionLifecyclePage ExecutionService::lifecycle_snapshot(
    std::uint64_t after_cursor, std::uint32_t limit) const {
  return lifecycle_telemetry_->snapshot(after_cursor, limit);
}

/** @copydoc ExecutionService::estimate_cpu_run_resources */
ResourceVector ExecutionService::estimate_cpu_run_resources(
    const ReadyTaskSubmission& representative, int total_task_count,
    CpuRunResourceDemand run_resource_demand) const {
  unsigned int configured_workers = 0U;
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (pool_->configured_workers == 0U || pool_->workers.empty()) {
      throw std::logic_error(
          "ExecutionService worker count is not configured.");
    }
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
    configured_workers = pool_->configured_workers;
  }
  return calculate_cpu_run_admission(
             configured_workers, representative.metadata().graph_identity(),
             total_task_count,
             representative.metadata().qos().maximum_parallelism,
             run_resource_demand)
      .resources;
}

/** @copydoc ExecutionService::prepare_run */
PreparedExecutionRun ExecutionService::prepare_run(
    ExecutionHostContext& host, const std::string& execution_type,
    std::vector<ReadyTaskSubmission> initial_submissions, int total_task_count,
    CpuRunResourceDemand run_resource_demand) {
  if (!is_execution_type(execution_type)) {
    throw std::invalid_argument(
        "ExecutionService requires a known private execution route.");
  }
  if (total_task_count <= 0 || initial_submissions.empty()) {
    throw std::invalid_argument(
        "ExecutionService requires a nonempty active Run batch.");
  }
  if (initial_submissions.size() > static_cast<std::size_t>(total_task_count)) {
    throw std::invalid_argument(
        "ExecutionService initial ready count exceeds total task count.");
  }
  const bool route_metal_registered =
      execution_type == "gpu_pipeline" &&
      pool_->device_executors.contains(DeviceBackend::Metal);
  const ComputeRunId run_id = initial_submissions.front().metadata().run_id();
  const ReadyTaskSubmission& representative = initial_submissions.front();
  const execution::DeviceCompletionSeed completion_seed(
      representative.metadata().graph_instance_id().value(),
      representative.metadata().target_node_id(),
      representative.metadata().supersession().key.request_intent(),
      representative.metadata().supersession().generation.value(),
      representative.identity().run_id().value(),
      representative.identity().local_task_id().value());
  ComputeRunLease service_lease = initial_submissions.front().lease_;
  std::unique_ptr<PreparedExecutionSharedReservation[]>
      supplemental_reservations;
  std::optional<ComputeRunLease> payload_cleanup_lease;
  for (const ReadyTaskSubmission& submission : initial_submissions) {
    if (submission.metadata().run_id() != run_id) {
      throw std::invalid_argument(
          "ExecutionService initial batch mixes multiple Runs.");
    }
    if (submission.resource_demand() != run_resource_demand.task) {
      throw std::invalid_argument(
          "ExecutionService initial batch resource declaration mismatch.");
    }
    if (!route_inventory_exposes_device(execution_type, route_metal_registered,
                                        submission.metadata().device())) {
      throw std::invalid_argument(
          "ExecutionService submission device is unavailable on its route.");
    }
  }

  unsigned int configured_workers = 0U;
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (pool_->configured_workers == 0U || pool_->workers.empty()) {
      throw std::logic_error(
          "ExecutionService worker count is not configured.");
    }
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
    configured_workers = pool_->configured_workers;
  }

  CpuRunAdmissionEstimate admission = calculate_cpu_run_admission(
      configured_workers,
      initial_submissions.front().metadata().graph_identity(), total_task_count,
      initial_submissions.front().metadata().qos().maximum_parallelism,
      run_resource_demand);
  std::optional<ResourceLedger::Reservation> reservation;
  const ResourceLedger::ReservationSettlementObserver settlement_observer =
      service_lease.begin_resource_settlement_observation(
          *lifecycle_telemetry_);
  {
    try {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      if (pool_->stopping) {
        throw std::logic_error("ExecutionService is stopping.");
      }
      reservation = pool_->try_reserve_for_policy(
          admission.resources,
          initial_submissions.front().metadata().qos().service_class,
          settlement_observer);
    } catch (...) {
      service_lease.cancel_resource_settlement_observation();
      throw;
    }
  }
  if (!reservation.has_value()) {
    service_lease.cancel_resource_settlement_observation();
    throw GraphError(
        GraphErrc::ComputeError,
        "ExecutionService policy/ledger cannot admit the complete Run.");
  }
  service_lease.commit_resource_settlement_observation();

  if (run_resource_demand.supplemental_retained_reservation_count != 0U) {
    supplemental_reservations =
        std::make_unique<PreparedExecutionSharedReservation[]>(
            run_resource_demand.supplemental_retained_reservation_count);
    payload_cleanup_lease.emplace(service_lease);
  }

  auto run = std::make_shared<RunState>(
      run_id, std::move(admission.policy_graph_identity),
      std::move(admission.policy_graph_key),
      initial_submissions.front().metadata().qos(), host, execution_type,
      route_metal_registered, total_task_count, run_resource_demand.task,
      admission.ready_bytes_per_task,
      admission.execution_retained_bytes_per_task,
      std::move(payload_cleanup_lease), std::move(*reservation),
      std::move(supplemental_reservations),
      run_resource_demand.supplemental_retained_reservation_count);

  const std::weak_ptr<RunState> weak_run(run);
  ComputeRunCancellationRegistration cancellation_registration =
      service_lease.register_cancellation_notification(
          [this, weak_run](ComputeRunCancellationReason reason) noexcept {
            if (const std::shared_ptr<RunState> accepted_run =
                    weak_run.lock()) {
              cancel_run(accepted_run, reason);
            }
          });

  std::vector<std::shared_ptr<QueueEntry>> staged_entries;
  staged_entries.reserve(initial_submissions.size());
  for (ReadyTaskSubmission& submission : initial_submissions) {
    staged_entries.push_back(make_queue_entry(run, std::move(submission)));
  }
  const std::size_t staged_submission_size = initial_submissions.size();
  const std::size_t staged_submission_capacity = initial_submissions.capacity();
  release_initial_submission_storage(initial_submissions);
  observe_initial_submission_storage(
      admission.resources, staged_submission_size, staged_submission_capacity,
      initial_submissions);

  BoundedReadyStore::PreparedBatch batch;
  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    if (pool_->configured_workers == 0U || pool_->workers.empty()) {
      throw std::logic_error(
          "ExecutionService worker count is not configured.");
    }
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
    if (pool_->ready_store.contains_run_id(run_id)) {
      throw std::logic_error("ExecutionService Run id is already active.");
    }
    batch = pool_->ready_store.prepare_initial_batch(run, staged_entries);
  }

  auto state = std::make_unique<PreparedExecutionRunState>();
  state->owner = this;
  state->run = std::move(run);
  state->batch = std::move(batch);
  state->completion_seed = completion_seed;
  state->cancellation_registration = std::move(cancellation_registration);
  return PreparedExecutionRun(std::move(state));
}

/** @copydoc ExecutionService::prepare_shared_reservation */
PreparedExecutionSharedReservation ExecutionService::prepare_shared_reservation(
    const ComputeRunLease& run_lease, std::uint64_t retained_memory_bytes) {
  if (retained_memory_bytes == 0U) {
    throw std::invalid_argument(
        "ExecutionService shared reservation requires retained memory.");
  }
  ComputeRunLease service_lease(run_lease);
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (pool_->configured_workers == 0U || pool_->workers.empty()) {
      throw std::logic_error(
          "ExecutionService worker count is not configured.");
    }
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
  }

  RetainedMemoryEstimator complete_demand(
      "ExecutionService shared reservation envelope");
  complete_demand.add_bytes(retained_memory_bytes);
  complete_demand.add_objects<PreparedExecutionSharedReservationState>();
  complete_demand.add_bytes(
      ResourceLedger::reservation_state_retained_memory_bytes());
  const ResourceVector resources{0U, complete_demand.bytes(), 0U, 0U, 0U};
  const ResourceLedger::ReservationSettlementObserver settlement_observer =
      service_lease.begin_resource_settlement_observation(
          *lifecycle_telemetry_);
  std::optional<ResourceLedger::Reservation> reservation;
  {
    try {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      if (pool_->stopping) {
        throw std::logic_error("ExecutionService is stopping.");
      }
      reservation = pool_->try_reserve_for_policy(
          resources, run_lease.descriptor().qos().service_class,
          settlement_observer);
    } catch (...) {
      service_lease.cancel_resource_settlement_observation();
      throw;
    }
  }
  if (!reservation.has_value()) {
    service_lease.cancel_resource_settlement_observation();
    throw GraphError(
        GraphErrc::ComputeError,
        "ExecutionService policy/ledger cannot admit shared Run ownership.");
  }
  service_lease.commit_resource_settlement_observation();

  auto state = std::make_unique<PreparedExecutionSharedReservationState>(
      std::move(service_lease), std::move(*reservation));
  return PreparedExecutionSharedReservation(std::move(state));
}

/** @copydoc ExecutionService::acquire_operation_execution */
OperationExecutionLease ExecutionService::acquire_operation_execution(
    const ComputeRunLease& run_lease,
    const OperationExecutionConstraints& constraints,
    ReadyTaskResourceDemand demand) {
  if (demand.work_units == 0U) {
    throw std::invalid_argument(
        "Direct operation execution requires positive work units.");
  }
  if ((!constraints.reentrant || constraints.maximum_parallelism != 0U) &&
      constraints.implementation_identity == 0U) {
    throw std::invalid_argument(
        "Direct operation caps require a nonzero implementation identity.");
  }
  if (constraints.exclusive_key.size() >
          OperationExecutionConstraints::kExclusiveKeyMaxBytes ||
      constraints.exclusive_key.find('\0') != std::string::npos) {
    throw std::invalid_argument(
        "Direct operation execution has an invalid exclusive key.");
  }

  ComputeRunLease service_lease(run_lease);
  auto state = std::make_unique<OperationExecutionLeaseState>(
      *this, std::move(service_lease), constraints);
  const ResourceVector resources =
      direct_operation_execution_resources(state->constraints, demand);
  OperationExecutionLeaseState* const state_ptr = state.get();
  const OperationExecutionConstraints& retained_constraints =
      state->constraints;

  {
    std::unique_lock<std::mutex> lock(pool_->mutex);
    while (!pool_->operation_gate.can_start(retained_constraints)) {
      if (pool_->stopping) {
        throw std::logic_error("ExecutionService is stopping.");
      }
      const std::uint64_t observed_epoch = pool_->worker_notification_epoch;
      (void)pool_->ready_cv.wait_for(
          lock, std::chrono::milliseconds(50),
          [this, state_ptr, observed_epoch]() {
            return pool_->stopping ||
                   pool_->worker_notification_epoch != observed_epoch ||
                   pool_->operation_gate.can_start(state_ptr->constraints);
          });
      lock.unlock();
      if (state->run_lease.observe_cancellation().has_value()) {
        throw GraphError(
            GraphErrc::ComputeError,
            "ComputeRun cancelled while waiting for operation admission.");
      }
      lock.lock();
    }
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
    if (!pool_->operation_gate.try_start(retained_constraints)) {
      throw std::logic_error(
          "Operation startability changed under the service lock.");
    }
    state->gate_started = true;
    pool_->advance_worker_notification_epoch();
  }
  pool_->ready_cv.notify_all();

  if (state->run_lease.observe_cancellation().has_value()) {
    throw GraphError(
        GraphErrc::ComputeError,
        "ComputeRun cancelled before operation resource admission.");
  }

  const ResourceLedger::ReservationSettlementObserver settlement_observer =
      state->run_lease.begin_resource_settlement_observation(
          *lifecycle_telemetry_);
  std::optional<ResourceLedger::Reservation> reservation;
  try {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
    reservation = pool_->try_reserve_for_policy(
        resources, state->run_lease.descriptor().qos().service_class,
        settlement_observer);
  } catch (...) {
    state->run_lease.cancel_resource_settlement_observation();
    throw;
  }
  if (!reservation.has_value()) {
    state->run_lease.cancel_resource_settlement_observation();
    throw GraphError(
        GraphErrc::ComputeError,
        "ExecutionService policy/ledger cannot admit direct operation.");
  }
  state->reservation.emplace(std::move(*reservation));
  state->run_lease.commit_resource_settlement_observation();
  return OperationExecutionLease(std::move(state));
}

#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
/**
 * @copydoc ExecutionService::direct_operation_gate_can_start_for_testing
 */
bool ExecutionService::direct_operation_gate_can_start_for_testing(
    const OperationExecutionConstraints& constraints) {
  std::lock_guard<std::mutex> lock(pool_->mutex);
  return pool_->operation_gate.can_start(constraints);
}
#endif

}  // namespace ps::compute
