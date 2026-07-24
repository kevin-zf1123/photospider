/**
 * @file kernel_close_test_access.hpp
 * @brief Declares test-only Kernel close-generation observation hooks.
 */
#pragma once

#if defined(PHOTOSPIDER_INTERNAL_KERNEL_CLOSE_TESTING)

namespace ps::testing {

/**
 * @brief Stable close boundary observed by repository-only integration tests.
 *
 * @throws Nothing for value construction and comparison.
 * @note These events expose no production or installed API surface.
 */
enum class KernelCloseTestEvent {
  /** @brief Exact owner was selected before lifecycle linearization. */
  OwnerSelectedBeforeLifecycle,
  /** @brief Exact joiner was selected before waiting for the owner result. */
  JoinerSelectedBeforeWait,
};

/**
 * @brief Borrowed callback installed by one serialized Kernel close test.
 *
 * @throws Any exception selected by invoke().
 * @note The test must keep both this record and context alive until every
 * observed close caller has returned and the hook is cleared.
 */
struct KernelCloseTestHook final {
  /** @brief Opaque borrowed test context. */
  void* context = nullptr;
  /**
   * @brief Optional observer that may block or throw at an observed boundary.
   * @param context Borrowed value from this record.
   * @param event Exact owner/joiner boundary.
   * @return Nothing.
   * @throws Any test-selected exception unchanged.
   */
  void (*invoke)(void* context, KernelCloseTestEvent event) = nullptr;
};

/**
 * @brief Replaces the borrowed process-global close hook.
 * @param hook Nullable serialized-test owner.
 * @return Nothing.
 * @throws Nothing.
 * @note Publication is atomic; tests still serialize replacement and join all
 * callbacks before destroying the borrowed record.
 */
void set_kernel_close_test_hook(const KernelCloseTestHook* hook) noexcept;

/**
 * @brief Invokes the current test hook at one close boundary.
 * @param event Exact owner/joiner boundary.
 * @return Nothing when no hook is installed or the observer returns.
 * @throws Any observer exception unchanged.
 */
void notify_kernel_close_test_hook(KernelCloseTestEvent event);

}  // namespace ps::testing

#endif
