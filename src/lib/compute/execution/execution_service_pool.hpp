#pragma once

/**
 * @file execution_service_pool.hpp
 * @brief Private prepared-run, resource-pool, and direct-lease state.
 *
 * The definitions preserve the single pool-to-Run lock order and remain
 * source-private implementation details of ExecutionService.
 */

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "compute/execution/execution_ready_store.hpp"

namespace ps::compute {

/**
 * @brief Complete unpublished physical batch behind PreparedExecutionRun.
 *
 * @throws Nothing from destruction after successful construction.
 * @note Field order makes cancellation registration retire before detached
 * entries, then RunState/reservation. The owning service outlives every
 * admission candidate and prepared batch.
 */
struct PreparedExecutionRunState final {
  /** @brief Exact service that created and may publish this state. */
  ExecutionService* owner = nullptr;
  /** @brief Matching physical Run and root reservation owner. */
  std::shared_ptr<ExecutionService::RunState> run;
  /** @brief Fully detached ready-store map/list publication nodes. */
  ExecutionService::BoundedReadyStore::PreparedBatch batch;
  /** @brief Representative exact lineage observed immediately before publish.
   */
  std::optional<execution::DeviceCompletionSeed> completion_seed;
  /** @brief Cancellation cleanup active through publication and settlement. */
  ComputeRunCancellationRegistration cancellation_registration;
};

/**
 * @brief Complete retained-only root behind one shared phase reservation.
 *
 * @throws Nothing from destruction after successful construction.
 * @note Declaration order releases the reservation before its Run lease. The
 * owning service outlives every prepared shared reservation.
 */
struct PreparedExecutionSharedReservationState final {
  /**
   * @brief Captures one exact Run lease and ledger root.
   * @param active_run_lease Matching Run settlement owner.
   * @param active_reservation Retained-only physical authority.
   * @throws Nothing.
   * @note Declaration order makes reservation release precede lease release.
   */
  PreparedExecutionSharedReservationState(
      ComputeRunLease active_run_lease,
      ResourceLedger::Reservation active_reservation) noexcept
      : run_lease(std::move(active_run_lease)),
        reservation(std::move(active_reservation)) {}

  /** @brief Matching Run retained through physical settlement. */
  ComputeRunLease run_lease;
  /** @brief Retained-only ledger root released before run_lease. */
  ResourceLedger::Reservation reservation;
};
namespace execution_service_detail {

/**
 * @brief Low-frequency fallback for grant releases outside service callbacks.
 *
 * @note Ordinary enqueue, completion, cancellation, policy replacement, and
 * shutdown transitions wake workers through a notification epoch. This bound
 * prevents an unobservable external child-grant release from stranding work
 * while keeping an exhausted candidate out of a busy retry loop.
 */
constexpr std::chrono::milliseconds kGrantRetryBackoff{50};

/**
 * @brief Resolves the bounded process execution-worker request once.
 * @param requested Zero for automatic resolution or an exact value in `[1,8]`.
 * @param detected Platform hardware concurrency, possibly zero.
 * @return Exact positive worker count in `[1,8]`.
 * @throws std::invalid_argument when `requested` exceeds the public bound.
 */
inline unsigned int resolve_execution_worker_count(unsigned int requested,
                                                   unsigned int detected) {
  if (requested > kExecutionWorkerRequestMax) {
    throw std::invalid_argument(
        "ExecutionService CPU worker count must be in [0,8].");
  }
  if (requested != 0U) {
    return requested;
  }
  return std::min(kExecutionWorkerRequestMax, std::max(1U, detected));
}

/**
 * @brief Validates and subtracts protected interactive admission headroom.
 * @param limits Immutable five-dimensional ledger capacity.
 * @param headroom Component-wise capacity reserved from Throughput Runs.
 * @return General-capacity ceiling available to Throughput Runs.
 * @throws std::invalid_argument when any headroom dimension exceeds its limit.
 * @note The returned vector is policy configuration, not resource authority.
 */
inline ResourceVector calculate_general_capacity(
    const ResourceVector& limits, const ResourceVector& headroom) {
  if (!resources_fit(headroom, limits)) {
    throw std::invalid_argument(
        "ExecutionService interactive headroom exceeds resource limits.");
  }
  return ResourceVector{
      limits.cpu_slots - headroom.cpu_slots,
      limits.retained_memory_bytes - headroom.retained_memory_bytes,
      limits.scratch_bytes - headroom.scratch_bytes,
      limits.ready_entries - headroom.ready_entries,
      limits.ready_bytes - headroom.ready_bytes,
  };
}

/**
 * @brief Tracks built-in Throughput root reservations against general quota.
 *
 * @throws std::system_error when its transaction mutex cannot be locked.
 * @note This accounting owns no physical capacity. `ResourceLedger` remains
 * the sole authority and retains this observer until the matching root vector
 * is physically returned after both parent and child-grant ownership end.
 */
class ThroughputReservationAccount final
    : public ResourceLedger::ReservationReleaseObserver {
 public:
  /**
   * @brief Fixes the policy-only ceiling for the service lifetime.
   * @param capacity Complete `limits - interactive_headroom` vector.
   * @throws Nothing.
   */
  explicit ThroughputReservationAccount(ResourceVector capacity) noexcept
      : capacity_(capacity) {}

  /** @copydoc
   * ResourceLedger::ReservationReleaseObserver::release_transaction_mutex */
  std::mutex& release_transaction_mutex() noexcept override { return mutex_; }

  /** @copydoc
   * ResourceLedger::ReservationReleaseObserver::on_reservation_released */
  void on_reservation_released(
      const ResourceVector& released) noexcept override {
    if (!resources_fit(released, reserved_)) {
      std::terminate();
    }
    reserved_ = ResourceVector{
        reserved_.cpu_slots - released.cpu_slots,
        reserved_.retained_memory_bytes - released.retained_memory_bytes,
        reserved_.scratch_bytes - released.scratch_bytes,
        reserved_.ready_entries - released.ready_entries,
        reserved_.ready_bytes - released.ready_bytes,
    };
  }

  /**
   * @brief Computes one prospective Throughput charge without mutation.
   * @param resources Complete candidate Run vector.
   * @return Next class-owned total, or null on overflow/quota exhaustion.
   * @throws Nothing.
   * @note Caller holds `release_transaction_mutex()`.
   */
  std::optional<ResourceVector> checked_charge(
      const ResourceVector& resources) const noexcept {
    const std::optional<ResourceVector> after =
        checked_add_resources(reserved_, resources);
    if (!after.has_value() || !resources_fit(*after, capacity_)) {
      return std::nullopt;
    }
    return after;
  }

  /**
   * @brief Commits a prevalidated charge after ledger reservation succeeds.
   * @param charged Exact value returned by `checked_charge()`.
   * @return Nothing.
   * @throws Nothing; a non-monotonic value terminates.
   * @note Caller holds `release_transaction_mutex()` and the ledger has already
   * committed the matching root reservation with this object as observer.
   */
  void commit_charge(const ResourceVector& charged) noexcept {
    if (!resources_fit(reserved_, charged) ||
        !resources_fit(charged, capacity_)) {
      std::terminate();
    }
    reserved_ = charged;
  }

  /**
   * @brief Copies fixed capacity and current Throughput-owned commitments.
   * @return Immutable policy-only diagnostic; no authority is minted.
   * @throws std::system_error when transaction locking fails.
   */
  ExecutionThroughputReservationSnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ExecutionThroughputReservationSnapshot{capacity_, reserved_};
  }

 private:
  /** @brief Serializes Throughput check/commit with exact root release. */
  mutable std::mutex mutex_;

  /** @brief Immutable policy quota excluding configured headroom. */
  const ResourceVector capacity_;

  /** @brief Active built-in Throughput root vectors only. */
  ResourceVector reserved_;
};

/**
 * @brief Tests whether a fixed registry executes one complete device identity.
 *
 * The current registry exposes one provisional device label per backend.
 * Therefore each populated slot represents ordinal zero; later ordinals and
 * Vulkan have no executable registry representation in this service.
 *
 * @param device_executors Frozen process-owned registry to inspect.
 * @param device Complete candidate ledger identity.
 * @return True only for a registered matching ordinal-zero executor.
 * @throws Nothing.
 * @note This observation neither mutates the registry nor creates an account.
 */
inline bool registry_executes_device_id(
    const execution::DeviceExecutorRegistry& device_executors,
    DeviceId device) noexcept {
  if (device.ordinal() != 0U) {
    return false;
  }
  switch (device.backend()) {
    case DeviceBackend::CPU:
      return false;
    case DeviceBackend::Metal:
      return device_executors.contains(Device::GPU_METAL);
    case DeviceBackend::CUDA:
      return device_executors.contains(Device::GPU_CUDA);
    case DeviceBackend::Vulkan:
      return false;
    case DeviceBackend::NPU:
      return device_executors.contains(Device::ASIC_NPU);
  }
  return false;
}

/**
 * @brief Validates device-limit candidates and removes non-executable accounts.
 *
 * First, the complete caller-supplied list is checked for CPU identities and
 * duplicates so filtering cannot hide invalid composition. Second, limits
 * whose complete `DeviceId` has no matching executor in the already frozen
 * registry are erased before `ResourceLedger` construction.
 *
 * @param resource_limits Mutable composition limits owned by construction.
 * @param device_executors Frozen registry defining executable device identity.
 * @return Nothing.
 * @throws std::invalid_argument for CPU or duplicate device candidates.
 * @note The operation allocates no storage, registers no executor, and leaves
 * an executor without a configured limit unable to reserve device capacity.
 */
inline void retain_executable_device_limits(
    ExecutionResourceLimits& resource_limits,
    const execution::DeviceExecutorRegistry& device_executors) {
  for (std::size_t index = 0U; index < resource_limits.device_limits.size();
       ++index) {
    const DeviceId device = resource_limits.device_limits[index].device;
    if (device.backend() == DeviceBackend::CPU) {
      throw std::invalid_argument(
          "ResourceLedger device limits must not contain CPU.");
    }
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (resource_limits.device_limits[previous].device == device) {
        throw std::invalid_argument(
            "ResourceLedger device limits contain a duplicate DeviceId.");
      }
    }
  }

  const auto first_unavailable = std::remove_if(
      resource_limits.device_limits.begin(),
      resource_limits.device_limits.end(),
      [&device_executors](const DeviceResourceLimit& limit) noexcept {
        return !registry_executes_device_id(device_executors, limit.device);
      });
  resource_limits.device_limits.erase(first_unavailable,
                                      resource_limits.device_limits.end());
}

}  // namespace execution_service_detail

/**
 * @brief Owns fixed compute/I/O workers, bounded stores, registries, and
 * ledgers.
 *
 * @throws std::bad_alloc from container growth and worker creation staging.
 * @note One mutex defines queue-to-Run lock order: pool mutex is acquired
 * before a Run mutex whenever both are needed.
 */
class ExecutionService::PoolState final {
 public:
  /**
   * @brief Creates one unconfigured execution domain with immutable limits.
   * @param resource_limits Complete Host, ready-store, compute-I/O, and device
   * limits whose device accounts already match the fixed registry.
   * @param device_executors Complete fixed device registry.
   * @param telemetry Stable physical counter owner.
   * @param policy_observer Stable non-owning service callback/binding observer.
   * @throws std::invalid_argument when interactive headroom exceeds a limit or
   * either compute-I/O limit is zero.
   * @throws std::bad_alloc or std::system_error when ledger or worker state
   * cannot be constructed.
   */
  PoolState(ExecutionResourceLimits resource_limits,
            execution::DeviceExecutorRegistry device_executors,
            ExecutionLifecycleTelemetry& telemetry,
            policy::PolicyLifecycleObserver policy_observer)
      : registry(policy::PolicyRegistry::process_instance()),
        device_executors(std::move(device_executors)),
        compute_io_executor(execution::ComputeIoExecutorLimits{
            resource_limits.compute_io_task_limit,
            resource_limits.compute_io_planned_bytes_limit}),
        interactive_binding(registry.create_binding(
            "interactive", PolicyClass::Interactive, 1U, policy_observer)),
        throughput_binding(registry.create_binding(
            "throughput", PolicyClass::Throughput, 1U, policy_observer)),
        throughput_reservations(std::make_shared<ThroughputReservationAccount>(
            calculate_general_capacity(resource_limits.resource_vector(),
                                       resource_limits.interactive_headroom))),
        ledger(resource_limits.resource_vector(),
               std::move(resource_limits.device_limits)),
        ready_store(resource_limits.ready_entries, resource_limits.ready_bytes,
                    telemetry, operation_gate) {
    interactive_binding->mark_service_published();
    throughput_binding->mark_service_published();
  }

  /**
   * @brief Applies one built-in policy ceiling before ordinary ledger commit.
   * @param resources Complete checked root vector requested by one Run/owner.
   * @param service_class Explicit policy class; no intent inference occurs.
   * @param settlement_observer Non-owning exact Run settlement callback.
   * @return Ledger-minted reservation, or null without mutation when either
   * policy ceiling or authoritative capacity is unavailable.
   * @throws std::bad_alloc or std::system_error from ledger admission.
   * @note Caller holds `mutex`. Throughput check, ledger commit, and class
   * charge are serialized by the account transaction mutex; exact root release
   * takes the same lock before returning capacity and debiting the class. The
   * policy mints no token; `ledger` remains the sole physical authority.
   */
  std::optional<ResourceLedger::Reservation> try_reserve_for_policy(
      const ResourceVector& resources, ComputeRunQosClass service_class,
      ResourceLedger::ReservationSettlementObserver settlement_observer) {
    if (service_class == ComputeRunQosClass::Interactive) {
      return ledger.try_reserve(resources, nullptr, settlement_observer);
    }

    std::lock_guard<std::mutex> transaction_lock(
        throughput_reservations->release_transaction_mutex());
    const std::optional<ResourceVector> after =
        throughput_reservations->checked_charge(resources);
    if (!after.has_value()) {
      return std::nullopt;
    }
    std::optional<ResourceLedger::Reservation> reservation = ledger.try_reserve(
        resources, throughput_reservations, settlement_observer);
    if (!reservation.has_value()) {
      return std::nullopt;
    }
    throughput_reservations->commit_charge(*after);
    return reservation;
  }

  /**
   * @brief Publishes one worker-relevant state transition under `mutex`.
   * @return Nothing.
   * @throws Nothing.
   * @note Every caller holds `mutex` and subsequently notifies `ready_cv`.
   * Unsigned wrap remains safe because grant-blocked waits also use a bounded
   * fallback; observing exactly one full 64-bit lap cannot strand a worker.
   */
  void advance_worker_notification_epoch() noexcept {
    ++worker_notification_epoch;
  }

  /**
   * @brief Copies the current immutable binding for one explicit class.
   * @param service_class Run QoS class already chosen by Host arbitration.
   * @return Shared invocation/context/DSO lease.
   * @throws Nothing; caller holds `mutex` and invalid enums terminate.
   */
  std::shared_ptr<policy::PolicyBinding> binding_for(
      ComputeRunQosClass service_class) const noexcept {
    switch (service_class) {
      case ComputeRunQosClass::Interactive:
        return interactive_binding;
      case ComputeRunQosClass::Throughput:
        return throughput_binding;
    }
    std::terminate();
  }

  /** @brief Process registry shared by all execution-service instances. */
  policy::PolicyRegistry& registry;

  /**
   * @brief Fixed process-owned device executors destroyed after worker join.
   */
  execution::DeviceExecutorRegistry device_executors;

  /**
   * @brief Independent process I/O worker with private task/byte accounting.
   */
  execution::ComputeIoExecutor compute_io_executor;

  /** @brief Serializes preparation/publication of policy replacements only. */
  mutable std::mutex policy_mutation_mutex;

  /** @brief Current Interactive context/generation/DSO lease. */
  std::shared_ptr<policy::PolicyBinding> interactive_binding;

  /** @brief Current Throughput context/generation/DSO lease. */
  std::shared_ptr<policy::PolicyBinding> throughput_binding;

  /**
   * @brief Non-authoritative quota for active built-in Throughput reservations.
   */
  const std::shared_ptr<ThroughputReservationAccount> throughput_reservations;

  /** @brief Serializes fixed configuration, queues, and active Run registry. */
  mutable std::mutex mutex;

  /** @brief Wakes fixed workers when ready work or shutdown is published. */
  std::condition_variable ready_cv;

  /** @brief Monotonic worker-relevant publication/completion generation. */
  std::uint64_t worker_notification_epoch = 0U;

  /** @brief Sole host-authoritative resource mint for this service. */
  ResourceLedger ledger;

  /** @brief Exact-identity and exclusive-key start ownership. */
  OperationStartGate operation_gate;

  /** @brief Sole policy-aware entry/byte-bounded ready-store owner. */
  BoundedReadyStore ready_store;

  /** @brief Private route discovery plus allocation-free start/finish state. */
  execution::PhysicalExecutionRoutes physical_routes;

  /** @brief Fixed CPU workers followed by one GPU-pipeline worker. */
  std::vector<std::thread> workers;

  /** @brief Frozen worker count, or zero before complete configuration. */
  unsigned int configured_workers = 0U;

  /** @brief True after explicit shutdown requests worker-loop exit. */
  bool stopping = false;

  /** @brief True while one control thread owns physical shutdown progress. */
  bool shutdown_in_progress = false;

  /** @brief True after routes, workers, bindings, and telemetry stop. */
  bool shutdown_complete = false;

  /** @brief Wakes repeated shutdown callers after the owner completes. */
  std::condition_variable shutdown_cv;
};

/**
 * @brief Complete direct callback resource/gate ownership behind one lease.
 *
 * @throws Nothing from destruction; trusted release failure terminates.
 * @note The service outlives every request lease. Resource capacity returns
 * before the operation gate opens and workers are notified. The retained
 * constraints key is charged by its actual copied capacity plus the trailing
 * null before admission.
 */
struct OperationExecutionLeaseState final {
  /**
   * @brief Stages allocation-owned state before gate/resource acquisition.
   * @param active_owner Exact execution service.
   * @param active_run_lease Matching Run lease.
   * @param active_constraints Exact implementation constraints.
   * @throws std::bad_alloc when exclusive-key copying allocates.
   */
  OperationExecutionLeaseState(
      ExecutionService& active_owner, ComputeRunLease active_run_lease,
      const OperationExecutionConstraints& active_constraints)
      : owner(&active_owner),
        run_lease(std::move(active_run_lease)),
        constraints(active_constraints) {}

  /**
   * @brief Returns reservation and operation ownership exactly once.
   * @throws Nothing; synchronization or ownership inconsistency terminates.
   */
  ~OperationExecutionLeaseState() noexcept {
    if (!gate_started) {
      return;
    }
    try {
      {
        std::lock_guard<std::mutex> lock(owner->pool_->mutex);
        reservation.reset();
        owner->pool_->operation_gate.finish(constraints);
        gate_started = false;
        owner->pool_->advance_worker_notification_epoch();
      }
      owner->pool_->ready_cv.notify_all();
    } catch (...) {
      std::terminate();
    }
  }

  /** @brief Exact service owning ledger and gate state. */
  ExecutionService* owner = nullptr;
  /** @brief Matching Run retained through resource settlement. */
  ComputeRunLease run_lease;
  /** @brief Frozen exact-identity and exclusive-key declaration. */
  OperationExecutionConstraints constraints;
  /** @brief Direct CPU/retained/scratch root reservation. */
  std::optional<ResourceLedger::Reservation> reservation;
  /** @brief Whether gate ownership must be released. */
  bool gate_started = false;
};

namespace execution_service_detail {

/**
 * @brief Calculates key-independent retained bytes for one direct lease.
 * @return Checked lease-state and ledger-reservation structural bytes.
 * @throws GraphError when retained-memory arithmetic overflows.
 * @note Declared operation bytes and the actual retained key payload are added
 * separately so callers can verify exact string charging.
 */
inline std::uint64_t direct_operation_fixed_retained_memory_bytes() {
  RetainedMemoryEstimator retained(
      "ExecutionService direct operation fixed envelope");
  retained.add_objects<OperationExecutionLeaseState>();
  retained.add_bytes(ResourceLedger::reservation_state_retained_memory_bytes());
  return retained.bytes();
}

/**
 * @brief Calculates the complete ledger vector for one direct operation lease.
 * @param constraints Exact identity and exclusive-key value retained by the
 * already-created lease state.
 * @param demand Additional operation retained/scratch declaration.
 * @return One CPU slot plus checked mandatory and declared retained/scratch.
 * @throws GraphError when retained-memory arithmetic overflows.
 * @note The returned value owns no gate, Run, reservation, or callback.
 */
inline ResourceVector direct_operation_execution_resources(
    const OperationExecutionConstraints& constraints,
    ReadyTaskResourceDemand demand) {
  RetainedMemoryEstimator retained(
      "ExecutionService direct operation envelope");
  retained.add_bytes(demand.retained_memory_bytes);
  retained.add_bytes(direct_operation_fixed_retained_memory_bytes());
  retained.add_string_payload(constraints.exclusive_key);
  return ResourceVector{1U, retained.bytes(), demand.scratch_bytes, 0U, 0U};
}

}  // namespace execution_service_detail

}  // namespace ps::compute
