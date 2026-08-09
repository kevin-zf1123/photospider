/**
 * @file single_tenant_job_service_test_access.hpp
 * @brief Exposes read-only source-private Job worker ownership test seams.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>

#include "server/single_tenant_job_service.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {

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
 * @note This source-private header is consumed only by maintained tests and
 * does not alter the installed ABI.
 */
class SingleTenantJobServiceTestAccess final {
 public:
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
