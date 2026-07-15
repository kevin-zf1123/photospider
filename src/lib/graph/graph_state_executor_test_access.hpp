#pragma once

#include <cstddef>

namespace ps::testing {

/**
 * @brief Lifecycle checkpoints published by a test-enabled graph-state lane.
 * @throws Nothing for value construction and comparison.
 * @note Production builds do not include this private test-access contract.
 */
enum class GraphStateExecutorTestEvent {
  /** @brief The sole worker entered its executor loop. */
  WorkerStarted,
  /** @brief One submission was appended to the bounded FIFO. */
  TaskQueued,
  /** @brief One queued task became the sole active task. */
  TaskStarted,
  /** @brief The active task finished and the lane became idle. */
  TaskFinished,
  /** @brief One close caller reached lifecycle coordination. */
  CloseCallerWaiting,
  /** @brief The drained worker left its executor loop. */
  WorkerStopped,
  /** @brief One closer completed the worker join. */
  Closed,
  /** @brief Failed-close rollback created one replacement worker. */
  Reopened,
};

/**
 * @brief Allocation-free bounded-lane state captured at one locked checkpoint.
 * @throws Nothing for aggregate construction and scalar access.
 * @note Counts describe one executor. `queued_tasks` excludes `active_tasks`;
 *       callbacks receive a borrowed snapshot valid only for the call.
 */
struct GraphStateExecutorTestSnapshot {
  /** @brief Checkpoint that produced this snapshot. */
  GraphStateExecutorTestEvent event =
      GraphStateExecutorTestEvent::WorkerStarted;
  /** @brief Fixed maximum number of waiting tasks. */
  std::size_t queue_capacity = 0;
  /** @brief Tasks waiting in FIFO storage. */
  std::size_t queued_tasks = 0;
  /** @brief Tasks executing on the worker; always zero or one. */
  std::size_t active_tasks = 0;
  /** @brief Executor-owned worker loops currently alive; zero or one. */
  std::size_t worker_threads = 0;
};

/**
 * @brief Borrowed observer for deterministic bounded-lane tests.
 * @throws Nothing for aggregate construction.
 * @note Tests serialize installation and retain both hook and context until all
 *       affected lane callbacks have completed.
 */
struct GraphStateExecutorTestHook {
  /** @brief Borrowed test context that outlives the installed hook. */
  void* context = nullptr;

  /**
   * @brief Observes one exact executor snapshot.
   * @param context Borrowed context supplied by the installing test.
   * @param snapshot Borrowed snapshot captured while the executor is locked.
   * @return Nothing.
   * @throws Nothing; callbacks must not throw, allocate, block, or re-enter an
   *         executor.
   */
  void (*notify)(void* context,
                 const GraphStateExecutorTestSnapshot& snapshot) noexcept =
      nullptr;
};

/**
 * @brief Installs or clears the bounded-lane observer.
 * @param hook Borrowed hook that outlives in-flight notifications, or nullptr.
 * @return Nothing.
 * @throws Nothing.
 */
void set_graph_state_executor_test_hook(
    const GraphStateExecutorTestHook* hook) noexcept;

/**
 * @brief Publishes one already-captured bounded-lane snapshot.
 * @param snapshot Snapshot borrowed for the callback duration.
 * @return Nothing.
 * @throws Nothing.
 * @note The executor invokes this while its state mutex is held; installed
 *       callbacks therefore must remain nonblocking and must not re-enter it.
 */
void notify_graph_state_executor_test_hook(
    const GraphStateExecutorTestSnapshot& snapshot) noexcept;

/**
 * @brief Deterministic hook for the unlocked Closed-to-notify test window.
 * @throws Nothing for aggregate construction.
 * @note The executor invokes this only after a join owner publishes `Closed`
 *       and releases its lifecycle mutex, but before it notifies close waiters.
 *       A test callback may re-enter `restart_after_close_failure()` to force
 *       the legal overlap that production scheduler-stop rollback can create.
 *       Tests serialize installation and keep context alive through every
 *       affected close.
 */
struct GraphStateExecutorClosePublishTestHook {
  /** @brief Borrowed test context that outlives the installed hook. */
  void* context = nullptr;

  /**
   * @brief Runs in the unlocked close-publication window.
   * @param context Borrowed context supplied by the installing test.
   * @return Nothing.
   * @throws Nothing; callbacks must contain every re-entry failure.
   */
  void (*before_waiter_notification)(void* context) noexcept = nullptr;
};

/**
 * @brief Installs or clears the unlocked close-publication hook.
 * @param hook Borrowed hook that outlives in-flight notifications, or nullptr.
 * @return Nothing.
 * @throws Nothing.
 */
void set_graph_state_executor_close_publish_test_hook(
    const GraphStateExecutorClosePublishTestHook* hook) noexcept;

/**
 * @brief Invokes the installed unlocked close-publication hook.
 * @return Nothing.
 * @throws Nothing.
 * @note Callers must not hold the executor lifecycle mutex. The callback may
 *       synchronously restart the fully joined lane.
 */
void notify_graph_state_executor_close_publish_test_hook() noexcept;

}  // namespace ps::testing
