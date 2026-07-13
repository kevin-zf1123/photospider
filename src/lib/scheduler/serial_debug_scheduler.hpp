// Photospider kernel: single-threaded built-in scheduler.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "photospider/scheduler/scheduler.hpp"

namespace ps {

/**
 * @brief Executes planned work synchronously on the submitting thread.
 *
 * The scheduler owns only inline completion/exception accounting and a borrowed
 * `SchedulerHostContext`. It does not own graph, cache, task-executor, or image
 * state. Handle batches run in input order and stop entering new callbacks
 * after the first exception is captured.
 *
 * @throws Synchronization, validation, overflow, and captured task exceptions
 *         according to the individual method contract.
 * @note Lifecycle calls are externally serialized. Submitted task handles are
 *       borrowed only for the duration of the synchronous call.
 */
class SerialDebugScheduler final : public IScheduler {
 public:
#if defined(PHOTOSPIDER_INTERNAL_SCHEDULER_TESTING)
  /**
   * @brief Immutable test snapshot of synchronized serial batch state.
   * @throws Nothing.
   * @note This source-level seam is absent from production target definitions.
   */
  struct TestingState {
    /** @brief Currently published nonzero batch epoch. */
    std::uint64_t active_epoch = 0U;
    /** @brief Logical tasks still expected by the active batch. */
    int tasks_to_complete = 0;
    /** @brief Entered nonzero-epoch callbacks borrowing any batch executor. */
    std::size_t borrowed_in_flight = 0U;
    /** @brief Epoch-zero callbacks executing. */
    std::size_t uncancellable_in_flight = 0U;
    /** @brief True when the active batch captured an exception. */
    bool has_exception = false;
    /** @brief True when the public completion wait predicate is satisfied. */
    bool completion_ready = false;
  };

  /**
   * @brief Copies synchronized batch state for deterministic focused tests.
   * @return Immutable state snapshot.
   * @throws std::system_error if locking the state mutex fails.
   * @note The snapshot observes every returned field under one mutex hold.
   */
  TestingState testing_state() const;
#endif

  /**
   * @brief Constructs one detached, stopped serial scheduler.
   * @throws std::system_error if synchronization primitives cannot initialize.
   * @note Construction allocates no graph, task-executor, or worker resource.
   */
  SerialDebugScheduler() = default;

  /**
   * @brief Stops the scheduler without owning or deleting its host context.
   * @throws Nothing.
   * @note Destruction performs the same idempotent lifecycle publication as
   *       `shutdown()` when the scheduler remains running.
   */
  ~SerialDebugScheduler() noexcept override;

  /**
   * @brief Prevents copying borrowed host and synchronous batch state.
   * @param other Scheduler whose ownership cannot be duplicated.
   * @throws Nothing because the operation is deleted.
   * @note Every instance owns one independent synchronization domain.
   */
  SerialDebugScheduler(const SerialDebugScheduler& other) = delete;

  /**
   * @brief Prevents copy assignment of borrowed host and batch state.
   * @param other Scheduler whose state cannot replace this instance.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   * @note Assignment could otherwise invalidate in-flight callback fences.
   */
  SerialDebugScheduler& operator=(const SerialDebugScheduler& other) = delete;

  /**
   * @brief Borrows the host context used for callback attribution and traces.
   * @param host Host-owned context that outlives shutdown and detach.
   * @return Nothing.
   * @throws Nothing.
   * @note The serial scheduler acquires no graph or runtime ownership.
   */
  void attach(SchedulerHostContext& host) noexcept override;

  /**
   * @brief Clears the borrowed host context after shutdown.
   * @return Nothing.
   * @throws Nothing.
   * @note Lifecycle serialization guarantees no callback uses the cleared
   *       pointer after this method returns.
   */
  void detach() noexcept override;

  /**
   * @brief Publishes the serial scheduler as ready for inline submission.
   * @return Nothing.
   * @throws Nothing.
   * @note No worker or queue resource is created; batch state is replaced only
   *       by a later initial submission.
   */
  void start() noexcept override;

  /**
   * @brief Stops admission and wakes completion waiters.
   * @return Nothing.
   * @throws Nothing.
   * @note Externally serialized lifecycle prevents overlap with an entered
   *       inline callback; the borrowed host remains attached.
   */
  void shutdown() noexcept override;

  /**
   * @brief Returns the stable built-in scheduler type name.
   * @return Owned `serial_debug` type name.
   * @throws std::bad_alloc if string construction cannot allocate.
   * @note The returned value aliases no scheduler or host storage.
   */
  std::string name() const override;

  /**
   * @brief Builds a lifecycle and successful-callback diagnostic snapshot.
   * @return Owned human-readable statistics text.
   * @throws std::bad_alloc if stream or result allocation fails.
   * @note The diagnostic is observational and not a control protocol.
   */
  std::string get_stats() const override;

  /**
   * @brief Reads the externally visible serial lifecycle state.
   * @return True after start and before shutdown.
   * @throws Nothing.
   * @note Acquire ordering observes the release-published lifecycle update.
   */
  bool is_running() const noexcept override;

  /**
   * @brief Owned callback used by internal compatibility helpers.
   * @throws std::bad_alloc when callable state construction cannot allocate.
   * @note The public runtime accepts the same callback ownership shape.
   */
  using Task = SchedulerTaskRuntime::Task;

  /**
   * @brief Priority alias used by built-in scheduler helpers and tests.
   * @throws Nothing.
   * @note Serial execution preserves input order for both priority labels.
   */
  using TaskPriority = SchedulerTaskPriority;

  /**
   * @brief Runs an internal callback batch synchronously.
   * @param tasks Callback batch moved into this call.
   * @param total_task_count Completion count for the complete planned batch.
   * @param priority Priority label; serial order is unchanged by priority.
   * @return Nothing.
   * @throws std::logic_error if the scheduler is not running.
   * @throws std::invalid_argument if the count is negative or smaller than the
   *         number of valid callbacks.
   * @throws std::system_error if internal synchronization fails.
   * @note Validation precedes epoch/exception/counter publication. Callback
   *       exceptions enter first-exception transport. This non-virtual helper
   *       is not part of the public scheduler SDK.
   */
  void submit_initial_tasks(std::vector<Task>&& tasks, int total_task_count,
                            TaskPriority priority = TaskPriority::Normal);

  /**
   * @brief Runs a valid borrowed-handle batch synchronously and atomically.
   * @param handles Dispatcher-owned handles borrowed for this call only.
   * @param total_task_count Nonnegative completion count that covers every
   *        valid handle.
   * @param priority Priority label; serial order is unchanged.
   * @return Nothing.
   * @throws std::logic_error if the scheduler is not running.
   * @throws std::invalid_argument if the count is negative or smaller than the
   *         number of valid handles.
   * @throws std::system_error if internal synchronization fails.
   * @note Validation finishes before the active epoch, first exception, and
   *       completion state are replaced. Rejection leaves the active batch
   *       unchanged and never enters a borrowed executor.
   */
  void submit_initial_task_handles(
      std::vector<TaskHandle>&& handles, int total_task_count,
      TaskPriority priority = TaskPriority::Normal) override;

  /**
   * @brief Runs one internal worker-ready callback synchronously.
   * @param task Callback to execute.
   * @param priority Priority label; serial order is unchanged.
   * @return Nothing.
   * @throws std::system_error if internal synchronization fails.
   * @note This non-virtual helper is not part of the public scheduler SDK.
   */
  void submit_ready_task_from_worker(
      Task&& task, TaskPriority priority = TaskPriority::Normal);

  /**
   * @brief Runs worker-released borrowed handles inline in input order.
   * @param handles Newly ready dispatcher-owned handles.
   * @param priority Observed label that does not change serial order.
   * @return Nothing.
   * @throws std::system_error if synchronized state access fails.
   * @note Calls outside this scheduler's active callback are ignored. Handle
   *       exceptions enter first-exception transport and stop later entries.
   */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      TaskPriority priority = TaskPriority::Normal) override;

  /**
   * @brief Runs one caller-owned callback synchronously on the calling thread.
   * @param task Callback state transferred into this call; empty work is
   *        ignored.
   * @param priority Observed label that does not change serial execution.
   * @param epoch Optional batch epoch; absence selects the active epoch and
   *        zero denotes uncancellable compatibility work.
   * @return Nothing.
   * @throws std::system_error if synchronized state access fails.
   * @note Stale/failed epochs are rejected before callback entry. Callback
   *       exceptions are captured and rethrown by `wait_for_completion()`.
   */
  void submit_ready_task_any_thread(
      Task&& task, TaskPriority priority = TaskPriority::Normal,
      std::optional<std::uint64_t> epoch = std::nullopt) override;

  /**
   * @brief Waits for logical completion and every entered callback to settle.
   * @return Nothing after successful completion or shutdown.
   * @throws The exact first callback or task-executor exception.
   * @throws std::system_error if condition-variable waiting fails.
   * @note Repeated waits retain the active exception until a new initial batch
   *       replaces it.
   */
  void wait_for_completion() override;

  /**
   * @brief Publishes the first non-null exception for the current call epoch.
   * @param error Exception identity to retain; null is ignored.
   * @return Nothing.
   * @throws std::system_error if synchronized state access fails.
   * @note Stale nonzero callbacks and duplicate publishers mutate no state.
   */
  void set_exception(std::exception_ptr error) override;

  /**
   * @brief Adds positive dynamically discovered work to the active count.
   * @param delta Positive count increment; nonpositive values are ignored.
   * @return Nothing.
   * @throws std::overflow_error if the current-epoch result exceeds `INT_MAX`.
   * @throws std::system_error if synchronized state access fails.
   * @note A stale nonzero callback cannot mutate the newer batch count.
   */
  void inc_tasks_to_complete(int delta) override;

  /**
   * @brief Retires one logical task with a zero completion floor.
   * @return Nothing.
   * @throws std::system_error if synchronized state access fails.
   * @note Stale nonzero callbacks and decrements after zero are no-ops; every
   *       call wakes completion waiters after synchronized inspection.
   */
  void dec_tasks_to_complete() override;

  /**
   * @brief Publishes one trace through the borrowed host context.
   * @param action Stable scheduler action.
   * @param node_id Associated backend node id, or -1 when unavailable.
   * @return Nothing.
   * @throws Nothing.
   * @note Detached calls are ignored; nested callback TLS supplies worker and
   *       epoch attribution.
   */
  void log_event(SchedulerTraceAction action, int node_id) noexcept override;

 private:
  /**
   * @brief Tests and claims one inline callback for its published epoch.
   * @param epoch Callback epoch; zero denotes uncancellable work.
   * @return True when the callback may enter.
   * @throws std::system_error if locking the state mutex fails.
   * @note Nonzero stale callbacks and callbacks after first exception are
   *       rejected before user code is entered.
   */
  bool try_begin_work(std::uint64_t epoch);

  /**
   * @brief Retires one callback and conditionally publishes its exception.
   * @param epoch Epoch captured when the callback entered.
   * @param error Callback exception, or null after success.
   * @return Nothing.
   * @throws std::system_error if locking the state mutex fails.
   * @note Every entered nonzero callback retires the cross-epoch borrowed
   *       executor fence exactly once. A stale callback cannot mutate the
   *       newer batch's exception or completion count.
   */
  void finish_work(std::uint64_t epoch, std::exception_ptr error);

  /**
   * @brief Resolves this scheduler's callback epoch on the calling thread.
   * @return Current epoch, or zero outside this scheduler's callback.
   * @throws Nothing.
   * @note Nested serial callbacks restore the prior epoch at scope exit.
   */
  std::uint64_t current_call_epoch() const noexcept;

  /** @brief Borrowed host context from attach until detach. */
  SchedulerHostContext* host_context_ = nullptr;

  /** @brief True after start and before shutdown. */
  std::atomic<bool> running_{false};

  /** @brief Protects active epoch, exception, counters, and in-flight state. */
  mutable std::mutex state_mutex_;

  /** @brief Serializes initial batch publication transactions. */
  std::mutex initial_submission_mutex_;

  /** @brief Wakes waiters after completion, failure, or shutdown. */
  std::condition_variable completion_cv_;

  /** @brief Inline completion count for the active synchronous batch. */
  int inline_tasks_to_complete_ = 0;

  /** @brief Entered nonzero-epoch callbacks borrowing any batch executor. */
  std::size_t borrowed_in_flight_tasks_ = 0U;

  /** @brief Epoch-zero callbacks currently executing. */
  std::size_t uncancellable_in_flight_tasks_ = 0U;

  /** @brief First exception captured for the active synchronous batch. */
  std::exception_ptr inline_exception_;

  /** @brief Nonzero epoch assigned to the active synchronous batch. */
  std::uint64_t active_epoch_ = 0U;

  /** @brief Number of callbacks that completed without throwing. */
  std::atomic<std::uint64_t> tasks_executed_{0};
};

}  // namespace ps
