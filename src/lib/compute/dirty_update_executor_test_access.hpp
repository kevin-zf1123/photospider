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
 * @brief Test-product callbacks for two distinct dirty planning boundaries.
 *
 * @throws Nothing for aggregate construction.
 * @note The owning test retains this hook and its context until synchronous
 * dirty preparation and its post-plan notification return. Each callback may
 * throw synchronously, but its Graph access is constrained by the callback's
 * documented lock phase and borrowed-object lifetime.
 */
struct DirtyPostPlanTestHook final {
  /** @brief Borrowed test context that outlives the installed hook. */
  void* context = nullptr;

  /**
   * @brief Observes one completed plan before active-operation revalidation.
   * @param context Borrowed context supplied by the installing test.
   * @return Nothing.
   * @throws Any exception selected by the deterministic test observer.
   * @note The caller has released the Graph planning mutex. The callback may
   * mutate OpRegistry for route-revalidation tests, but must not access,
   * mutate, or retain the GraphModel associated with the completed plan.
   */
  void (*notify)(void* context) = nullptr;

  /**
   * @brief Observes the retained request cone before dirty task selection.
   * @param context Borrowed context supplied by the installing test.
   * @param node_cache_plan Complete callback-free request-cone task shape with
   * planning-time cache observations used only as diagnostics or merge-base
   * facts by dirty execution.
   * @param graph Request-scoped planning Graph whose runtime cache state may be
   * mutated under the caller's planning mutex.
   * @return Nothing.
   * @throws Any exception selected by the deterministic test observer.
   * @note The callback may perform only controlled synchronous Graph/cache
   * mutation needed by the current test. It must not acquire the planning
   * mutex, retain either borrowed argument, or start work that outlives the
   * call.
   */
  void (*notify_node_cache_plan)(void* context,
                                 const ComputePlan& node_cache_plan,
                                 GraphModel& graph) = nullptr;
};

/**
 * @brief Installs or clears the current thread's dirty planning observer.
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
 * @param node_cache_plan Complete retained request-cone task shape and
 * planning-time cache observations.
 * @param graph Request-scoped planning Graph whose retained cone was formed.
 * @return Nothing.
 * @throws Any exception selected by the installed test observer.
 * @note A null hook or callback is a no-op. The caller holds the Graph planning
 * mutex; the callback may synchronously mutate request-scoped cache state but
 * must not re-enter that mutex or retain borrowed objects. No dirty/external
 * boundary selection has been formed.
 */
void notify_dirty_node_cache_plan_test_hook(const ComputePlan& node_cache_plan,
                                            GraphModel& graph);

}  // namespace ps::compute::testing
