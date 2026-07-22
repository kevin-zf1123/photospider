#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "compute/execution_service.hpp"

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
   * @brief Repository-test callback after one execution grant is staged.
   * @param context Opaque fixture state that outlives the observed Run.
   * @param candidate_id Nonzero ready-entry identity retained by the pin.
   * @param entry_version Nonzero nonreused entry version retained by the pin.
   * @param route_generation Immutable Run route generation retained by the pin.
   * @param execution_resources Exact staged child-grant resources.
   * @return True to force pre-commit rollback; false to continue the start.
   * @throws Nothing.
   */
  using ReservedStartRollbackObserver =
      bool (*)(void* context, std::uint64_t candidate_id,
               std::uint64_t entry_version, std::uint64_t route_generation,
               const ResourceVector& execution_resources) noexcept;

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
   * @brief Installs one observer on an isolated reserved-start boundary.
   * @param service Private service whose next starts are observed.
   * @param observer Allocation-free decision callback, or null to disable.
   * @param context Opaque callback context, or null when disabling.
   * @return Nothing.
   * @throws Nothing.
   * @note Installation and clearing happen outside concurrent configuration;
   * the observer itself runs while the private pool and Run locks are held.
   */
  static void set_reserved_start_rollback_observer(
      compute::ExecutionService& service,
      ReservedStartRollbackObserver observer, void* context) noexcept {
    service.reserved_start_rollback_observer_context_ = context;
    service.reserved_start_rollback_observer_ = observer;
  }

  /**
   * @brief Clears the reserved-start observer after the observed Run settles.
   * @param service Isolated service whose observer is removed.
   * @return Nothing.
   * @throws Nothing.
   */
  static void clear_reserved_start_rollback_observer(
      compute::ExecutionService& service) noexcept {
    service.reserved_start_rollback_observer_ = nullptr;
    service.reserved_start_rollback_observer_context_ = nullptr;
  }

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
