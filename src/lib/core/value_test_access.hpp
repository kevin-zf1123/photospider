#pragma once

#if !defined(PHOTOSPIDER_INTERNAL_VALUE_ALLOCATION_TESTING)
#error "value_test_access.hpp is available only in BUILD_TESTING builds"
#endif

#include <cstddef>

namespace ps::testing {

/**
 * @brief Snapshot of one thread-local BufferHandle allocation observation.
 *
 * @throws Nothing for ordinary construction, copy, or destruction.
 * @note The counters cover only ControlBlock owners created after the current
 *       thread arms the source-private seam. They never expose an allocation
 *       address, runtime identity, or production ownership handle.
 */
struct BufferControlBlockAllocationObservation final {
  /** @brief Whether the selected pre-ControlBlock boundary threw bad_alloc. */
  bool fired = false;

  /** @brief ControlBlock owners completed before the selected boundary. */
  std::size_t successful_allocations = 0U;

  /** @brief Observed owners destroyed before the snapshot was captured. */
  std::size_t released_allocations = 0U;
};

/**
 * @brief Arms one thread-local failure before a selected ControlBlock owner.
 *
 * @param allocation_number One-based BufferHandle CPU ControlBlock allocation
 *        number to fail; zero leaves failure injection disabled while starting
 *        an empty observation window.
 * @return Nothing.
 * @throws Nothing.
 * @note The one-shot failpoint is reached immediately before
 *       `std::make_shared<BufferHandle::ControlBlock>`. Concurrent threads own
 *       independent plans, and callers must clear the plan after the
 *       synchronous operation under test completes or throws.
 */
void arm_buffer_control_block_allocation_failure_for_testing(
    std::size_t allocation_number) noexcept;

/**
 * @brief Captures the calling thread's current allocation observation.
 *
 * @return Value snapshot of failpoint, successful-owner, and release counts.
 * @throws Nothing.
 * @note Observation does not clear the plan, allowing callers to snapshot
 *       complete stack unwinding before explicitly restoring normal behavior.
 */
BufferControlBlockAllocationObservation
buffer_control_block_allocation_observation_for_testing() noexcept;

/**
 * @brief Clears the calling thread's allocation failure and observation plan.
 *
 * @return Nothing.
 * @throws Nothing.
 * @note Other threads and already published BufferHandle owners are
 *       unaffected.
 */
void clear_buffer_control_block_allocation_failure_for_testing() noexcept;

}  // namespace ps::testing
