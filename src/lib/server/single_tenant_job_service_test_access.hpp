/**
 * @file single_tenant_job_service_test_access.hpp
 * @brief Exposes source-private Job submission and worker-ownership test seams.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>

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
 * @brief Read-only test access to private worker-thread ownership state.
 *
 * The seam observes only how many joinable worker handles remain owned by a
 * live service. It cannot mutate Job truth, start or stop work, publish a
 * report, or expose a worker handle.
 *
 * @throws Nothing except documented standard synchronization or arithmetic
 * failures.
 * @note This source-private interface exists only to support maintained tests.
 * Its definitions are compiled through the non-installed internal target and
 * do not alter the installed ABI.
 */
class SingleTenantJobServiceTestAccess final {
 public:
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
