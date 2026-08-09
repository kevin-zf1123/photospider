/**
 * @file single_tenant_job_service_test_access.hpp
 * @brief Exposes source-private identity, report, ownership, and observer
 * seams.
 *
 * Identity access reserves from caller-owned local atomic sequences. Job-state
 * access observes retained accepted records, worker access observes
 * service-owned thread counts without exposing handles, and one explicit
 * report seam drives deterministic stale-attempt fencing coverage.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "server/single_tenant_job_service.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {

/**
 * @brief Arms one scoped deterministic assignment-thread start failure.
 *
 * Construction arms the next `WorkerThreadRecord` start attempted by the
 * current thread. The injected path records that attempt's `JobId`, consumes
 * the arm, and raises `std::system_error` before any `std::thread` owns a
 * native handle. Destruction disarms an unused injection.
 *
 * @throws std::logic_error when another injection guard is already alive on
 * the current thread.
 * @note This source-private test seam is not installed and is not a product
 * contract. A guard must be used and destroyed on its constructing thread.
 * Different threads have independent arms, and one guard affects at most one
 * worker-start attempt.
 */
class ScopedWorkerThreadStartFailure final {
 public:
  /**
   * @brief Arms the next assignment-thread start on the current thread.
   * @throws std::logic_error when that thread already owns an active guard.
   */
  ScopedWorkerThreadStartFailure();

  /**
   * @brief Disarms any unconsumed failure injection on the current thread.
   * @throws Nothing.
   * @note The guard must be destroyed on the thread that constructed it.
   */
  ~ScopedWorkerThreadStartFailure() noexcept;

  /**
   * @brief Prevents duplicate ownership of one thread-local injection.
   * @param other Guard whose unique arm would otherwise be duplicated.
   * @throws Nothing because the operation is deleted.
   */
  ScopedWorkerThreadStartFailure(const ScopedWorkerThreadStartFailure& other) =
      delete;

  /**
   * @brief Prevents duplicate assignment of one thread-local injection.
   * @param other Guard whose unique arm would otherwise be duplicated.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ScopedWorkerThreadStartFailure& operator=(
      const ScopedWorkerThreadStartFailure& other) = delete;

  /**
   * @brief Keeps the thread-local capture address stable for guard lifetime.
   * @param other Guard whose capture address cannot be transferred.
   * @throws Nothing because the operation is deleted.
   */
  ScopedWorkerThreadStartFailure(ScopedWorkerThreadStartFailure&& other) =
      delete;

  /**
   * @brief Keeps the thread-local capture address stable during assignment.
   * @param other Guard whose capture address cannot be transferred.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ScopedWorkerThreadStartFailure& operator=(
      ScopedWorkerThreadStartFailure&& other) = delete;

  /**
   * @brief Returns the Job identity whose worker start consumed the arm.
   * @return Empty before consumption, otherwise the exact rolled-back JobId.
   * @throws Nothing.
   * @note The returned reference remains valid only for this guard's lifetime.
   */
  const std::optional<JobId>& attempted_job_id() const noexcept {
    return attempted_job_id_;
  }

 private:
  /** @brief Rolled-back Job identity captured before the injected exception. */
  std::optional<JobId> attempted_job_id_;
};

/**
 * @brief One mutex-consistent snapshot of private worker-thread ownership.
 * @throws Nothing for value operations.
 * @note Active records are still executing, completed records await reaper
 * transfer, and joining records have been moved to the reaper's unlocked join.
 */
struct WorkerThreadOwnershipSnapshot final {
  /** @brief Worker records that have not reached their completion tail. */
  std::size_t active = 0U;
  /** @brief Completed worker records still stored in the ownership map. */
  std::size_t completed = 0U;
  /** @brief Completed handles currently being joined outside the mutex. */
  std::size_t joining = 0U;

  /**
   * @brief Returns all worker handles still owned by the service/reaper.
   * @return Checked sum of active, completed, and joining counts.
   * @throws std::overflow_error if an impossible size_t sum overflows.
   */
  std::size_t total() const {
    if (active > std::numeric_limits<std::size_t>::max() - completed ||
        active + completed >
            std::numeric_limits<std::size_t>::max() - joining) {
      throw std::overflow_error("worker ownership snapshot overflowed");
    }
    return active + completed + joining;
  }
};

/**
 * @brief Mutex-consistent audit view of active reservation receipt ownership.
 * @throws Nothing for value operations.
 * @note Job-control owners cover active attempts and terminal release failures.
 * The stranded owner covers one rollback candidate with no durable Job owner.
 * This observation does not expose reservation identities or mutation access.
 */
struct QuotaReservationOwnershipSnapshot final {
  /** @brief Reservation owners retained by authoritative Job controls. */
  std::size_t job_controls = 0U;
  /** @brief Rollback-only owners retained by the service stranded slot. */
  std::size_t stranded = 0U;

  /**
   * @brief Returns all reservation receipts retained by the service.
   * @return Checked sum of Job-control and stranded owners.
   * @throws std::overflow_error if an impossible size_t sum overflows.
   */
  std::size_t total() const {
    if (job_controls > std::numeric_limits<std::size_t>::max() - stranded) {
      throw std::overflow_error("quota ownership snapshot overflowed");
    }
    return job_controls + stranded;
  }
};

/**
 * @brief Provides local identity, deterministic report, ownership, and
 * observer seams.
 *
 * Identity methods reserve from and therefore may modify only the
 * caller-supplied local atomic sequence. They do not read, reset, or otherwise
 * mutate production process-wide counters. Accepted-state methods observe
 * retained Job-record truth, worker methods observe handle ownership, and
 * quota methods observe reservation-receipt ownership. The observation methods
 * expose no handles or identities and do not mutate product state. The explicit
 * report method is the sole mutation seam and delegates to the exact production
 * fencing boundary without constructing alternate authority.
 *
 * @throws Exceptions are method-specific and documented below. They include
 * `std::invalid_argument` for invalid seam inputs, `std::overflow_error` for
 * identity saturation or checked arithmetic, `std::system_error` from
 * synchronization operations, and any exception raised by an identity
 * observer callback.
 * @note This source-private interface exists only to support maintained tests.
 * Its definitions are compiled through the non-installed internal target and
 * do not alter the installed ABI.
 */
class SingleTenantJobServiceTestAccess final {
 public:
  /**
   * @brief Reserves one production identity sequence value for a local test.
   * @param sequence Non-null caller-owned sequence, normally a local atomic.
   * @return Fresh nonzero value, including `UINT64_MAX` for the final
   * reservation.
   * @throws std::invalid_argument when `sequence` is null.
   * @throws std::overflow_error when the sequence is already saturated.
   * @note This source-private seam delegates to the exact helper used by
   * production identity minting. It never reads or mutates the real
   * process-wide identity counters and adds no product test state.
   */
  static std::uint64_t reserve_identity_sequence_value(
      std::atomic<std::uint64_t>* sequence);

  /**
   * @brief Pauses one local reservation after its initial atomic observation.
   * @param sequence Non-null caller-owned sequence, normally a local atomic.
   * @param after_initial_observation Nonempty callback invoked after the first
   * observation and before saturation checking or reservation linearization.
   * @return Fresh nonzero value when the sequence was not saturated.
   * @throws std::invalid_argument when either argument is invalid.
   * @throws std::overflow_error when the observed sequence is saturated.
   * @throws Any callback exception unchanged before this reservation attempt
   * mutates the sequence.
   * @note Production uses the same reservation implementation with a
   * stateless inline no-op. This deterministic source-private test seam adds
   * no global or product-owned mutable test state.
   */
  static std::uint64_t reserve_identity_with_observer(
      std::atomic<std::uint64_t>* sequence,
      const std::function<void()>& after_initial_observation);

  /**
   * @brief Injects one report at the exact private production fencing boundary.
   * @param service Live service whose current assignment may accept or fence
   * it.
   * @param expected Assignment identity owned by the simulated reporting
   * worker.
   * @param report Complete candidate report moved into production validation.
   * @return Nothing after production processing or stale-attempt fencing.
   * @throws Nothing; `SingleTenantJobService::apply_report` is fail-closed.
   * @note This deterministic source-private seam proves stale-attempt and
   * durable-mutation fail-stop fencing. It does not bypass any identity,
   * shape, cancellation, artifact, persistence, or quota validation.
   */
  static void inject_attempt_report(SingleTenantJobService& service,
                                    const AttemptIdentity& expected,
                                    JobAttemptReport report) noexcept {
    service.apply_report(expected, std::move(report));
  }

  /**
   * @brief Returns the number of accepted Job records retained by a service.
   * @param service Live service whose private accepted state is observed.
   * @return Exact mutex-consistent Job-record count.
   * @throws std::system_error for mutex synchronization failure.
   * @note Failed pre-acceptance submission must not increase this count.
   */
  static std::size_t accepted_job_count(const SingleTenantJobService& service) {
    std::lock_guard<std::mutex> lock(service.mutex_);
    return service.jobs_.size();
  }

  /**
   * @brief Reports whether journal commit ambiguity fail-stopped mutations.
   * @param service Live service whose private monotonic state is observed.
   * @return True after a published journal operation lost confirmation/ack.
   * @throws std::system_error for mutex synchronization failure.
   * @note This observation grants no recovery or mutation authority.
   */
  static bool journal_faulted(const SingleTenantJobService& service) {
    std::lock_guard<std::mutex> lock(service.mutex_);
    return service.journal_faulted_;
  }

  /**
   * @brief Reports whether artifact deletion fail-stopped durable mutation.
   * @param service Live service whose private monotonic state is observed.
   * @return True after manifest visibility became irreversible but deletion
   * durability, cleanup, acknowledgement, or quota coordination failed.
   * @throws std::system_error for mutex synchronization failure.
   * @note This observation grants no recovery, cleanup, or quota authority.
   */
  static bool artifact_erase_faulted(const SingleTenantJobService& service) {
    std::lock_guard<std::mutex> lock(service.mutex_);
    return service.artifact_erase_faulted_;
  }

  /**
   * @brief Reports whether artifact reconciliation fail-stopped mutation.
   * @param service Live service whose private monotonic state is observed.
   * @return True after manifest-visible lookup/revalidation, quota conversion,
   * or Succeeded journal reconciliation could not complete safely.
   * @throws std::system_error for mutex synchronization failure.
   * @note This observation grants no recovery, quota, or mutation authority.
   * The current active reservation or retained charge remains product truth.
   */
  static bool artifact_reconciliation_faulted(
      const SingleTenantJobService& service) {
    std::lock_guard<std::mutex> lock(service.mutex_);
    return service.artifact_reconciliation_faulted_;
  }

  /**
   * @brief Reports whether active-attempt release fail-stopped mutation.
   * @param service Live service whose private monotonic state is observed.
   * @return True after quota release raised before settlement mutation.
   * @throws std::system_error for mutex synchronization failure.
   * @note The exact owner remains on a terminal Job control or in the
   * rollback-only stranded slot until service destruction/restart.
   */
  static bool quota_release_faulted(const SingleTenantJobService& service) {
    std::lock_guard<std::mutex> lock(service.mutex_);
    return service.quota_release_faulted_;
  }

  /**
   * @brief Reports the combined durable-mutation fail-stop state.
   * @param service Live service whose private monotonic state is observed.
   * @return True for published Job-journal failure, irreversible artifact
   * deletion failure, manifest-visible artifact reconciliation failure, or
   * active-attempt release failure.
   * @throws std::system_error for mutex synchronization failure.
   * @note Query, lookup, and quota observation remain available; mutation and
   * worker progress are fenced until restart.
   */
  static bool durable_mutation_faulted(const SingleTenantJobService& service) {
    std::lock_guard<std::mutex> lock(service.mutex_);
    return service.durable_mutation_faulted_locked();
  }

  /**
   * @brief Captures every retained active-reservation receipt owner.
   * @param service Live service whose private ownership is observed.
   * @return Counts split between Job controls and rollback stranded storage.
   * @throws std::system_error for mutex synchronization failure.
   * @note The snapshot exposes no reservation id and grants no settlement
   * authority. Under release fail-stop its total matches active quota usage.
   */
  static QuotaReservationOwnershipSnapshot quota_reservation_ownership(
      const SingleTenantJobService& service) {
    std::lock_guard<std::mutex> lock(service.mutex_);
    QuotaReservationOwnershipSnapshot snapshot;
    for (const auto& entry : service.jobs_) {
      if (entry.second.reservation.has_value()) {
        ++snapshot.job_controls;
      }
    }
    snapshot.stranded = service.stranded_reservation_.has_value() ? 1U : 0U;
    return snapshot;
  }

  /**
   * @brief Captures active, completed, and joining worker ownership.
   * @param service Live service whose private ownership is observed.
   * @return Exact mutex-consistent ownership snapshot.
   * @throws std::system_error for mutex synchronization failure.
   */
  static WorkerThreadOwnershipSnapshot worker_thread_ownership(
      const SingleTenantJobService& service) {
    std::lock_guard<std::mutex> lock(service.mutex_);
    WorkerThreadOwnershipSnapshot snapshot;
    for (const auto& entry : service.workers_) {
      if (entry.second.completed) {
        ++snapshot.completed;
      } else {
        ++snapshot.active;
      }
    }
    snapshot.joining = service.workers_joining_;
    return snapshot;
  }

  /**
   * @brief Returns the number of worker handles retained by the service.
   * @param service Live service whose private ownership is observed.
   * @return Exact retained worker-handle count at one mutex-protected instant.
   * @throws std::system_error for mutex synchronization failure.
   * @throws std::overflow_error if the checked ownership total overflows.
   * @note A completed but unjoined `std::thread` remains included.
   */
  static std::size_t owned_worker_thread_count(
      const SingleTenantJobService& service) {
    return worker_thread_ownership(service).total();
  }

  /**
   * @brief Waits until retained worker ownership reaches a requested bound.
   * @param service Live service whose private ownership is observed.
   * @param maximum_count Inclusive upper bound accepted by the predicate.
   * @param timeout Nonnegative observer wait bound.
   * @return True when ownership reached the bound before timeout.
   * @throws std::invalid_argument for a negative timeout.
   * @throws std::system_error for condition-variable synchronization failure.
   * @note This is an observation-only test wait; it never performs reaping.
   */
  static bool wait_for_owned_worker_thread_count_at_most(
      const SingleTenantJobService& service, std::size_t maximum_count,
      std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
      throw std::invalid_argument("worker ownership wait timeout is negative");
    }
    std::unique_lock<std::mutex> lock(service.mutex_);
    return service.condition_.wait_for(lock, timeout, [&] {
      return service.workers_.size() + service.workers_joining_ <=
             maximum_count;
    });
  }
};

}  // namespace ps::server
