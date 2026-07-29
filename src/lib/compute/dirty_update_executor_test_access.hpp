#pragma once

#if !defined(PHOTOSPIDER_INTERNAL_DIRTY_UPDATE_TESTING)
#error "dirty_update_executor_test_access.hpp is test-product only"
#endif

namespace ps {
class GraphModel;
}

namespace ps::compute {
struct ComputePlan;
}

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

  /**
   * @brief Observes a retained node/cache plan before dirty task selection.
   * @param context Borrowed context supplied by the installing test.
   * @param node_cache_plan Complete callback-free task shape whose planning
   * cache observations have not yet been classified against dirty candidates.
   * @param graph Planning Graph whose runtime cache state may be mutated.
   * @return Nothing.
   * @throws Any exception selected by the deterministic test observer.
   * @note The caller holds the Graph planning mutex. The callback must perform
   * only synchronous runtime-state mutation and must not acquire that mutex.
   */
  void (*notify_node_cache_plan)(void* context,
                                 const ComputePlan& node_cache_plan,
                                 GraphModel& graph) = nullptr;
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

/**
 * @brief Notifies the current test thread before dirty task selection.
 * @param node_cache_plan Complete retained task shape and planning-time cache
 * observations.
 * @param graph Planning Graph whose node/cache plan was just retained.
 * @return Nothing.
 * @throws Any exception selected by the installed test observer.
 * @note A null hook or callback is a no-op. The caller holds the Graph planning
 * mutex and no dirty/external-boundary selection has been formed.
 */
void notify_dirty_node_cache_plan_test_hook(const ComputePlan& node_cache_plan,
                                            GraphModel& graph);

}  // namespace ps::compute::testing
