#include "graph/graph_state_executor.hpp"

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
#include <atomic>

#include "graph/graph_state_executor_test_access.hpp"  // NOLINT(build/include_subdir)
#endif

namespace ps {

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
namespace testing {
namespace {

/**
 * @brief Borrowed contention-observer pointer stored by the atomic seam.
 * @throws Nothing for alias use.
 */
using ContentionTestHookPtr = const GraphStateExecutorContentionTestHook*;

/**
 * @brief Process-local borrowed observer shared by test-enabled executors.
 * @throws Nothing for atomic initialization and pointer publication.
 * @note Tests serialize installation and remove the observer before destroying
 *       its hook or context.
 */
std::atomic<ContentionTestHookPtr> g_contention_test_hook{nullptr};

}  // namespace

/** @copydoc ps::testing::set_graph_state_executor_contention_test_hook */
void set_graph_state_executor_contention_test_hook(
    const GraphStateExecutorContentionTestHook* hook) noexcept {
  g_contention_test_hook.store(hook, std::memory_order_release);
}

/** @copydoc ps::testing::notify_graph_state_executor_contention_test_hook */
void notify_graph_state_executor_contention_test_hook() noexcept {
  const GraphStateExecutorContentionTestHook* hook =
      g_contention_test_hook.load(std::memory_order_acquire);
  if (hook != nullptr && hook->notify != nullptr) {
    hook->notify(hook->context);
  }
}

}  // namespace testing
#endif

/** @copydoc GraphStateExecutor::lock_task */
void GraphStateExecutor::lock_task(std::unique_lock<std::mutex>& lock) {
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
  if (!lock.try_lock()) {
    testing::notify_graph_state_executor_contention_test_hook();
    lock.lock();
  }
#else
  lock.lock();
#endif
}

}  // namespace ps
