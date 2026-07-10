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
  /** @brief Number of active device implementation values. */
  std::size_t implementation_count = 0;
  /** @brief Number of parallel active ownership revisions. */
  std::size_t revision_count = 0;
  /** @brief Whether every active revision is non-zero. */
  bool all_revisions_nonzero = true;
};

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
