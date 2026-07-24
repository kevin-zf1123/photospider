#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "compute/execution_service.hpp"
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
#include "compute/execution_service_test_probe.hpp"
#endif

namespace ps::testing {

/**
 * @brief Test-only access to deterministic ExecutionService staging cleanup.
 *
 * @note This private support header is compiled only by repository tests. It
 * exposes no Host, Policy ABI, plugin, or installed-library API.
 */
class ExecutionServiceTestAccess final {
 public:
  /**
   * @brief Repository-test callback for one production retirement boundary.
   * @param context Opaque fixture state that outlives the observed Run.
   * @param admitted_resources Complete vector reserved by production
   * admission.
   * @param staged_size Moved-from element count before retirement.
   * @param staged_capacity Backing capacity before retirement.
   * @param released_size Element count after retirement.
   * @param released_capacity Backing capacity after retirement.
   * @return Nothing.
   * @throws Nothing.
   */
  using InitialSubmissionStorageObserver = void (*)(
      void* context, const ResourceVector& admitted_resources,
      std::size_t staged_size, std::size_t staged_capacity,
      std::size_t released_size, std::size_t released_capacity) noexcept;

  /**
   * @brief Repository-test callback after one worker QueueEntry is destroyed.
   * @param context Opaque fixture state.
   * @param run_id Exact observed Run identity.
   * @return Nothing.
   * @throws Nothing.
   * @note The Run remains non-settleable through `in_flight` during callback.
   */
  using WorkerEntryRetirementObserver =
      void (*)(void* context, compute::ComputeRunId run_id) noexcept;

  /**
   * @brief Releases one initial-submission vector through the production seam.
   * @param submissions Initial values whose storage should be retired.
   * @return Nothing.
   * @throws Nothing.
   * @note Production calls the same boundary only after moving every value
   * into a staged queue entry and before publishing or waiting for the Run.
   */
  static void release_initial_submission_storage(
      std::vector<compute::ReadyTaskSubmission>& submissions) noexcept {
    compute::ExecutionService::release_initial_submission_storage(submissions);
  }

  /**
   * @brief Installs one observer on an isolated service instance.
   * @param service Private service whose next real Run is observed.
   * @param observer Allocation-free callback, or null to disable observation.
   * @param context Opaque callback context, or null when disabling.
   * @return Nothing.
   * @throws Nothing.
   * @note Installation and clearing must happen outside concurrent Run calls.
   */
  static void set_initial_submission_storage_observer(
      compute::ExecutionService& service,
      InitialSubmissionStorageObserver observer, void* context) noexcept {
    service.initial_submission_storage_observer_context_ = context;
    service.initial_submission_storage_observer_ = observer;
  }

  /**
   * @brief Clears the observer after every observed synchronous Run settles.
   * @param service Isolated service whose observer is removed.
   * @return Nothing.
   * @throws Nothing.
   */
  static void clear_initial_submission_storage_observer(
      compute::ExecutionService& service) noexcept {
    service.initial_submission_storage_observer_ = nullptr;
    service.initial_submission_storage_observer_context_ = nullptr;
  }

  /**
   * @brief Installs the deterministic worker-entry retirement observer.
   * @param service Isolated service whose worker is observed.
   * @param observer Allocation-free callback, or null to disable.
   * @param context Opaque context, or null when disabling.
   * @return Nothing.
   * @throws Nothing.
   */
  static void set_worker_entry_retirement_observer(
      compute::ExecutionService& service,
      WorkerEntryRetirementObserver observer, void* context) noexcept {
    service.worker_entry_retirement_observer_context_ = context;
    service.worker_entry_retirement_observer_ = observer;
  }

  /**
   * @brief Clears the worker-entry observer after the Run settles.
   * @param service Isolated service whose observer is removed.
   * @return Nothing.
   * @throws Nothing.
   */
  static void clear_worker_entry_retirement_observer(
      compute::ExecutionService& service) noexcept {
    service.worker_entry_retirement_observer_ = nullptr;
    service.worker_entry_retirement_observer_context_ = nullptr;
  }

#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
  /**
   * @brief Arms the separate test-product reserved-start rollback probe.
   * @param service Isolated service used only to document test ownership.
   * @return Nothing.
   * @throws Nothing.
   * @note The first child grant is discarded without invoking a callback. The
   * production archive contains no probe state or branch.
   */
  static void arm_reserved_start_rollback_probe(
      compute::ExecutionService& service) noexcept {
    (void)service;
    compute::testing::arm_reserved_start_rollback_probe_for_testing();
  }

  /**
   * @brief Copies the separate test-product reserved-start observation.
   * @param service Isolated service whose synchronous Run has settled.
   * @return First two attempts and total call count.
   * @throws Nothing.
   */
  static compute::testing::ReservedStartRollbackProbeSnapshot
  reserved_start_rollback_probe_snapshot(
      const compute::ExecutionService& service) noexcept {
    (void)service;
    return compute::testing::
        reserved_start_rollback_probe_snapshot_for_testing();
  }

  /**
   * @brief Disarms the separate test-product probe during cleanup.
   * @param service Isolated service whose test ownership is ending.
   * @return Nothing.
   * @throws Nothing.
   */
  static void disarm_reserved_start_rollback_probe(
      compute::ExecutionService& service) noexcept {
    (void)service;
    compute::testing::disarm_reserved_start_rollback_probe_for_testing();
  }
#endif

  /**
   * @brief Copies active built-in Throughput reservation charges.
   * @param service Isolated service under test.
   * @return Exact class-owned vector excluding Interactive owners.
   * @throws std::system_error when private accounting locking fails.
   * @note This is a non-authoritative test diagnostic; the ledger snapshot
   * remains the physical-capacity source of truth.
   */
  static ResourceVector throughput_reservation_snapshot(
      const compute::ExecutionService& service) {
    return service.throughput_reservation_snapshot_for_testing();
  }
};

}  // namespace ps::testing
