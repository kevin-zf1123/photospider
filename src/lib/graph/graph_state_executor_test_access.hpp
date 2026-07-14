#pragma once

namespace ps::testing {

/**
 * @brief Borrowed observer for deterministic executor-contention tests.
 *
 * @throws Nothing for aggregate construction.
 * @note Production builds do not include this private test-access contract.
 */
struct GraphStateExecutorContentionTestHook {
  /** @brief Borrowed test context that outlives the installed hook. */
  void* context = nullptr;

  /**
   * @brief Records that one executor task observed its mutex already locked.
   * @param context Borrowed context supplied by the installing test.
   * @return Nothing.
   * @throws Nothing; callbacks must not throw, block, or re-enter an executor.
   */
  void (*notify)(void* context) noexcept = nullptr;
};

/**
 * @brief Installs or clears the executor-contention observer.
 *
 * @param hook Borrowed hook that outlives in-flight notifications, or nullptr.
 * @return Nothing.
 * @throws Nothing.
 */
void set_graph_state_executor_contention_test_hook(
    const GraphStateExecutorContentionTestHook* hook) noexcept;

/**
 * @brief Notifies the installed observer after a real try_lock failure.
 *
 * @return Nothing.
 * @throws Nothing.
 * @note Notification occurs before the task performs its normal blocking lock.
 */
void notify_graph_state_executor_contention_test_hook() noexcept;

}  // namespace ps::testing
