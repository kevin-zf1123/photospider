/**
 * @file kernel_close_test_access.hpp
 * @brief Declares test-only Kernel lifecycle observation hooks.
 */
#pragma once

#if defined(PHOTOSPIDER_INTERNAL_KERNEL_CLOSE_TESTING)

namespace ps::testing {

/**
 * @brief Stable lifecycle boundary observed by repository-only integration
 * tests.
 *
 * @throws Nothing for value construction and comparison.
 * @note These events expose no production or installed API surface.
 */
enum class KernelCloseTestEvent {
  /** @brief Exact owner was selected before lifecycle linearization. */
  OwnerSelectedBeforeLifecycle,
  /** @brief Exact joiner was selected before waiting for the owner result. */
  JoinerSelectedBeforeWait,
  /**
   * @brief A failed generation was observed after releasing the graph
   * registry lock and immediately before waiting for retry eligibility.
   */
  RetryPendingBeforeWait,
  /**
   * @brief Owner drained the runtime immediately before atomic erase/success.
   */
  OwnerReadyToEraseAndPublish,
  /**
   * @brief Process publication closed before execution-service transition.
   *
   * @note Throwing from this checkpoint exercises the monotonic shutdown
   * fail-stop boundary after the recoverable caller preflight has completed.
   */
  ShutdownGateClosedBeforeTransition,
};

/**
 * @brief Borrowed callback installed by one serialized Kernel lifecycle test.
 *
 * @throws Any exception selected by invoke().
 * @note The test must keep both this record and context alive until every
 * observed lifecycle caller has returned and the hook is cleared. A joiner
 * observer exception is rethrown only after its exact generation result is
 * consumed. Throwing from OwnerReadyToEraseAndPublish or
 * ShutdownGateClosedBeforeTransition occurs after monotonic lifecycle state
 * changed and therefore exercises a production fail-stop boundary.
 */
struct KernelCloseTestHook final {
  /** @brief Opaque borrowed test context. */
  void* context = nullptr;
  /**
   * @brief Optional observer that may block or throw at an observed boundary.
   * @param context Borrowed value from this record.
   * @param event Exact close or process-shutdown boundary.
   * @return Nothing.
   * @throws Any test-selected exception unchanged.
   */
  void (*invoke)(void* context, KernelCloseTestEvent event) = nullptr;
};

/**
 * @brief Replaces the borrowed process-global Kernel lifecycle hook.
 * @param hook Nullable serialized-test owner.
 * @return Nothing.
 * @throws Nothing.
 * @note Publication is atomic; tests still serialize replacement and join all
 * callbacks before destroying the borrowed record.
 */
void set_kernel_close_test_hook(const KernelCloseTestHook* hook) noexcept;

/**
 * @brief Invokes the current test hook at one Kernel lifecycle boundary.
 * @param event Exact close or process-shutdown boundary.
 * @return Nothing when no hook is installed or the observer returns.
 * @throws Any observer exception unchanged.
 * @note Kernel consumes an already selected Joiner claim before propagating a
 * JoinerSelectedBeforeWait observer exception.
 */
void notify_kernel_close_test_hook(KernelCloseTestEvent event);

}  // namespace ps::testing

#endif
