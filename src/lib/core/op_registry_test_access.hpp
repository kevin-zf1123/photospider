#pragma once

#if !defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
#error "op_registry_test_access.hpp is available only in BUILD_TESTING builds"
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ps {

class OpRegistry;

namespace testing {

/**
 * @brief Reports device-value and revision-vector alignment for one key.
 *
 * @throws Nothing; the value owns no dynamic storage.
 * @note Tests use this count-only view to prove the registry never exposes
 *       callable objects through an internal inspection seam.
 */
struct OpRegistryDeviceOwnershipInspection {
  /** @brief Whether the implementation table contains the inspected key. */
  bool implementation_entry_present = false;
  /** @brief Whether the ownership table contains the inspected key. */
  bool ownership_entry_present = false;
  /** @brief Number of active device implementation values. */
  std::size_t implementation_count = 0;
  /** @brief Number of parallel active ownership revisions. */
  std::size_t revision_count = 0;
  /** @brief Whether every active revision is non-zero. */
  bool all_revisions_nonzero = true;
  /** @brief First active device revision, or zero when no device slot exists.
   */
  std::uint64_t first_device_revision = 0;
  /** @brief Revision currently owning the HP monolithic compatibility slot. */
  std::uint64_t monolithic_hp_revision = 0;
  /** @brief Revision currently owning the HP tiled compatibility slot. */
  std::uint64_t tiled_hp_revision = 0;
  /** @brief Revision currently owning the HP metadata slot. */
  std::uint64_t meta_hp_revision = 0;
};

/**
 * @brief Precise device-registration boundaries exposed to deterministic tests.
 *
 * @throws Nothing; this enum carries no owned state.
 * @note Production products neither compile this enum nor the corresponding
 *       thread-local failpoint state.
 */
enum class OpRegistryDeviceRegistrationFailpoint {
  /** @brief No injected failure. */
  None,
  /** @brief Fail immediately before constructing the stable device owner. */
  StableOwner,
  /** @brief Fail before constructing the CPU HP forwarding bridge. */
  CpuCompatibilityBridge,
};

/**
 * @brief Counts final host device-wrapper retirement and lock observations.
 *
 * @throws Nothing; construction and destruction operate only on atomics.
 * @note The test owning this value must outlive every wrapper that may report
 *       to it, serialize observer installation/removal with wrapper retirement,
 *       and clear the process-global observer before destruction. Observer
 *       scopes must not overlap.
 */
struct OpRegistryDeviceCallbackRetirementInspection {
  /** @brief Number of final retained device-wrapper destructions. */
  std::atomic<std::uint64_t> destructions{0};
  /** @brief Number of those destructions entered with registry lock ownership.
   */
  std::atomic<std::uint64_t> destructions_under_lock{0};
};

/**
 * @brief Arms one thread-local device-registration failure boundary.
 *
 * @param failpoint Boundary that will throw `std::bad_alloc` when reached.
 * @return Nothing.
 * @throws Nothing.
 * @note Installation resets the hit counter. Tests must observe exactly one hit
 *       so an unrelated allocation failure cannot satisfy the assertion.
 */
void set_op_registry_device_registration_failpoint(
    OpRegistryDeviceRegistrationFailpoint failpoint) noexcept;

/**
 * @brief Returns visits to the currently selected registration failpoint.
 *
 * @return Thread-local hit count since the last failpoint installation.
 * @throws Nothing.
 * @note A successful precise-failure test observes one hit before disarming.
 */
std::size_t op_registry_device_registration_failpoint_hits() noexcept;

/**
 * @brief Installs or clears the host device-wrapper retirement observer.
 *
 * @param inspection Externally owned counters, or `nullptr` to disable reports.
 * @return Nothing.
 * @throws Nothing; installation is one atomic pointer publication.
 * @note The seam observes final host wrapper retirement only; it neither owns
 *       callbacks nor changes plugin/library lifetime. `inspection` is a
 *       borrowed process-global pointer. The caller must serialize replacement
 *       or clearing with reports and keep it alive through every possible
 *       reporting wrapper destruction.
 */
void set_op_registry_device_callback_retirement_inspection(
    OpRegistryDeviceCallbackRetirementInspection* inspection) noexcept;

/**
 * @brief Records final retirement of one host-retained device callback wrapper.
 *
 * @param registry Registry whose current-thread lock token is inspected.
 * @return Nothing.
 * @throws Nothing; absent observers are ignored and reporting uses atomics
 * only.
 * @note The plugin loader calls this at the start of the host wrapper's final
 *       destructor, immediately before its plugin callback target is destroyed.
 */
void report_op_registry_device_callback_retirement_for_testing(
    const OpRegistry& registry) noexcept;

/**
 * @brief Installs or clears the test observer for registry lock contention.
 *
 * @param counter Externally owned counter incremented once when a lock attempt
 *        first observes another thread holding the registry lock, or `nullptr`
 *        to disable observation.
 * @return Nothing.
 * @throws Nothing; installation is one atomic pointer publication.
 * @note The counter must outlive every contending lock attempt and must be
 *       cleared only after those attempts have completed. The production ABI
 *       does not expose this BUILD_TESTING-only internal seam.
 */
void set_op_registry_contention_counter(
    std::atomic<std::uint64_t>* counter) noexcept;

/**
 * @brief Checks whether the calling thread currently owns a registry lock.
 *
 * @param registry Registry whose lock owner is inspected.
 * @return True only when the current thread owns at least one recursive level.
 * @throws Nothing; the check is one relaxed atomic load and pointer compare.
 * @note Callback-destructor tests use this deterministic signal instead of
 *       inferring lock release from elapsed time or absence of deadlock.
 */
bool op_registry_lock_held_by_current_thread_for_testing(
    const OpRegistry& registry) noexcept;

/**
 * @brief Inspects device implementation and ownership-vector alignment.
 *
 * @param registry Registry whose coherent state is inspected.
 * @param key Canonical operation key in `type:subtype` form.
 * @return Counts and non-zero revision status captured under one registry lock.
 * @throws Nothing; lookup and count inspection allocate no storage.
 * @note Missing implementation or ownership entries contribute a zero count,
 *       making stale ownership after whole-key unregister directly observable.
 */
OpRegistryDeviceOwnershipInspection
inspect_op_registry_device_ownership_for_testing(
    const OpRegistry& registry, const std::string& key) noexcept;

}  // namespace testing
}  // namespace ps
