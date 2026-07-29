#pragma once

#if !defined(PHOTOSPIDER_INTERNAL_DIRTY_UPDATE_TESTING)
#error "dirty_update_executor_test_access.hpp is test-product only"
#endif

namespace ps::compute::testing {

/**
 * @brief Test-product observer at the dirty plan/revalidation boundary.
 *
 * @throws Nothing for aggregate construction.
 * @note The owning test retains this hook and its context until synchronous
 * dirty preparation returns. The callback may mutate OpRegistry or throw, but
 * must not mutate the GraphModel whose planning lock has just been released.
 */
struct DirtyPostPlanTestHook final {
  /** @brief Borrowed test context that outlives the installed hook. */
  void* context = nullptr;

  /**
   * @brief Observes one completed plan before active-operation revalidation.
   * @param context Borrowed context supplied by the installing test.
   * @return Nothing.
   * @throws Any exception selected by the deterministic test observer.
   */
  void (*notify)(void* context) = nullptr;
};

/**
 * @brief Installs or clears the current thread's dirty post-plan observer.
 * @param hook Borrowed hook that outlives the synchronous prepare call, or
 * nullptr to clear observation.
 * @return Nothing.
 * @throws Nothing.
 * @note Thread-local ownership keeps independent focused tests isolated. The
 * production archive neither defines this symbol nor executes an observer
 * branch.
 */
void set_dirty_post_plan_test_hook(const DirtyPostPlanTestHook* hook) noexcept;

}  // namespace ps::compute::testing
