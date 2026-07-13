// Photospider built-in CPU work-stealing scheduler.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/scheduler/scheduler.hpp"
#include "photospider/scheduler/scheduler_task_runtime.hpp"

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
#include "scheduler/scheduler_exception_test_hooks.hpp"
#endif

namespace ps {

/**
 * @brief Dispatches ready callbacks through priority and work-stealing queues.
 *
 * The scheduler owns worker threads, global high/normal queues, per-worker
 * normal queues, batch epoch/counter state, and first-exception publication.
 * It does not own `SchedulerHostContext`, task graphs, cache state, or
 * executors behind
 * borrowed `TaskHandle` values. Batch handle publication is transactional:
 * workers cannot observe any handle until every queue insertion commits.
 *
 * @return Concrete `IScheduler` implementation with inherited task runtime.
 * @throws std::bad_alloc from explicit construction/start/submission methods.
 * @throws std::system_error from explicit worker lifecycle operations.
 * @note Lifecycle calls are externally serialized. A borrowed handle remains
 * valid only through the matching `wait_for_completion()` batch boundary.
 */
class CpuWorkStealingScheduler : public IScheduler {
 public:
  /**
   * @brief Configures a stopped CPU work-stealing scheduler.
   * @param num_workers Worker count, or zero to use hardware concurrency.
   * @return A stopped scheduler that owns no worker threads yet.
   * @throws Nothing directly; hardware concurrency failure falls back to one.
   * @note The host-context pointer remains null until `attach()`; no queue,
   * cache, or task-executor ownership is acquired by construction.
   */
  explicit CpuWorkStealingScheduler(unsigned int num_workers = 0);

  /**
   * @brief Stops and releases scheduler-owned worker resources.
   * @return Nothing.
   * @throws Nothing by destructor contract; valid built-in shutdown is expected
   * not to fail while joining scheduler-owned threads.
   * @note Borrowed host/task-handle targets must already satisfy their
   * lifecycle contracts when destruction begins.
   */
  ~CpuWorkStealingScheduler() override;

  /** @brief Prevents copying worker, queue, and borrowed-host ownership. */
  CpuWorkStealingScheduler(const CpuWorkStealingScheduler&) = delete;

  /** @brief Prevents copy assignment of worker and batch state. */
  CpuWorkStealingScheduler& operator=(const CpuWorkStealingScheduler&) = delete;

  // ---------------------------------------------------------------------------
  // IScheduler implementation
  // ---------------------------------------------------------------------------
  /**
   * @brief Attaches a borrowed host context for trace and task attribution.
   * @param host Host context that outlives shutdown and detach.
   * @return Nothing.
   * @throws Nothing.
   * @note Attachment transfers no graph, cache, scheduler, or runtime
   * ownership and must occur before start under runtime serialization.
   */
  void attach(SchedulerHostContext& host) override;

  /**
   * @brief Clears the borrowed host trace/context target.
   * @return Nothing.
   * @throws Nothing.
   * @note Detach does not stop workers; the owning host orders shutdown first.
   */
  void detach() override;
  /**
   * @brief Starts the configured worker pool with clean batch state.
   *
   * @return Nothing.
   * @throws std::bad_alloc if worker/local-queue storage cannot grow.
   * @throws std::system_error if a worker thread cannot be created.
   * @note Container capacity is staged before member publication. Partially
   * created workers use `worker_loop_active_`; the public `running_` flag stays
   * false until the complete worker vector is installed. If creation fails,
   * already-created workers observe stop, are joined, every queue/counter/array
   * is cleared, and the original exception propagates. Retry is supported;
   * start must not overlap a batch.
   */
  void start() override;
  /**
   * @brief Stops worker dispatch and joins all scheduler-owned threads.
   *
   * shutdown publishes the stop state under the same mutexes used by idle
   * worker and completion waiters, then wakes both condition variables before
   * joining workers. This preserves the scheduler lifecycle contract even when
   * the last task completes while an idle worker is transitioning into
   * condition-variable sleep.
   *
   * @throws std::system_error if mutex acquisition or joining a valid worker
   * thread fails.
   * @return Nothing.
   * @note The caller must not invoke shutdown from a scheduler-owned worker.
   * Pending queued callbacks are discarded after all workers have observed the
   * stop state and exited.
   */
  void shutdown() override;
  /**
   * @brief Returns the stable scheduler display name.
   * @return `CpuWorkStealingScheduler`.
   * @throws std::bad_alloc if result string allocation fails.
   * @note The name carries no lifecycle, queue, cache, or ownership state.
   */
  std::string name() const override;

  /**
   * @brief Formats a snapshot of worker, queue, and dispatch counters.
   * @return Human-readable scheduler statistics.
   * @throws std::bad_alloc if stream/string growth fails.
   * @note Values are relaxed diagnostic snapshots, not synchronization or
   * completion predicates.
   */
  std::string get_stats() const override;

  /**
   * @brief Reports whether worker dispatch is currently published as running.
   * @return Current acquire-loaded lifecycle state.
   * @throws Nothing.
   * @note True is published only after start creates the complete worker set.
   */
  bool is_running() const override;

  // Built-in callback helpers; production dispatch uses public handle batches.
  /** @brief Internal callback alias retained for built-in compute helpers. */
  using Task = SchedulerTaskRuntime::Task;
  /** @brief Internal priority alias shared with public runtime submissions. */
  using TaskPriority = SchedulerTaskPriority;

  /**
   * @brief Atomically submits an internal callback batch in a new epoch.
   *
   * The method stages the next epoch, inserts the complete batch while holding
   * the global visibility lock, and rolls every touched deque back to its
   * original size if any insertion throws. Only a complete queue commit
   * publishes epoch, logical/ready counters, statistics, and notification.
   *
   * @param tasks Immediately ready callbacks transferred to scheduler queues;
   *        empty callbacks are ignored.
   * @param total_task_count Nonnegative logical count covering every valid
   *        initial callback plus dynamically released work.
   * @param priority Initial queue priority.
   * @return Nothing.
   * @throws std::logic_error if the scheduler is not running.
   * @throws std::invalid_argument if the count is negative or smaller than the
   *         number of valid callbacks.
   * @throws std::bad_alloc if queue growth fails while storing callbacks.
   * @note Validation precedes epoch, exception, queue, and counter mutation.
   *       Empty valid batches publish a new settled epoch and notify waiters.
   */
  void submit_initial_tasks(std::vector<Task>&& tasks, int total_task_count,
                            TaskPriority priority = TaskPriority::Normal);

  /**
   * @brief Atomically submits an initial borrowed-handle batch.
   *
   * The handle path shares callback-path epoch, exception reset, completion,
   * and notification semantics. High priority uses the global FIFO; normal
   * work is distributed across per-worker queues under global-to-local lock
   * order, permitting allocation-free rollback before borrowed visibility.
   *
   * @param handles Dispatcher-owned handles; empty handles are ignored.
   * @param total_task_count Nonnegative logical count covering every valid
   *        initial handle.
   * @param priority Initial queue priority.
   * @return Nothing.
   * @throws std::logic_error if the scheduler is not running.
   * @throws std::invalid_argument if the count is negative or smaller than the
   *         number of valid handles.
   * @throws std::bad_alloc if queue growth fails while storing handles.
   * @note Validation precedes all active-batch state mutation. Handles borrow
   *       dispatcher executors through the matching wait settlement boundary.
   */
  void submit_initial_task_handles(
      std::vector<TaskHandle>&& handles, int total_task_count,
      TaskPriority priority = TaskPriority::Normal) override;

  /**
   * @brief Enqueues one newly ready callback from a scheduler worker.
   *
   * @param task Callback whose ownership moves into the scheduler queue.
   * @param priority Scheduling priority for the callback.
   * @return Nothing.
   * @throws std::bad_alloc if queue growth fails before publication.
   * @note Normal-priority publication holds the global predicate mutex before
   * the owning local-queue mutex, rejects claimed/stale epochs, and increments
   * `ready_task_count_` before releasing either lock.
   */
  void submit_ready_task_from_worker(
      Task&& task, TaskPriority priority = TaskPriority::Normal);

  /**
   * @brief Enqueues one newly ready task handle from a scheduler worker.
   *
   * @param handle Handle released after its dependency count reaches zero.
   * @param priority Scheduling priority for the handle.
   * @return Nothing.
   * @throws std::bad_alloc if queue growth fails before publication.
   * @note Normal-priority work prefers the current worker's local queue and
   * publishes with the global-to-local lock order used by the ready predicate.
   */
  void submit_ready_task_handle_from_worker(
      TaskHandle handle, TaskPriority priority = TaskPriority::Normal);

  /**
   * @brief Enqueues a newly ready handle batch from a scheduler worker.
   *
   * @param handles Handles produced by one dependency-release stage.
   * @param priority Scheduling priority shared by the handle batch.
   * @return Nothing.
   * @throws std::bad_alloc if queue growth fails.
   * @note The selected queue is restored to its original size on any insertion
   * failure. Ready/stat counters and notification publish only after commit, so
   * no borrowed executor can outlive a failed dirty dependency release.
   */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      TaskPriority priority = TaskPriority::Normal) override;

  /**
   * @brief Enqueues one newly ready callback from any thread.
   *
   * @param task Callback whose ownership moves into the global queue.
   * @param priority Scheduling priority for the callback.
   * @param epoch Optional originating batch epoch; the active epoch is used
   * when absent.
   * @return Nothing.
   * @throws std::bad_alloc if global queue growth fails before publication.
   * @note The global queue mutex serializes enqueue, ready-count increment,
   * exception-claim rejection, and stale-epoch rejection with cleanup.
   */
  void submit_ready_task_any_thread(
      Task&& task, TaskPriority priority = TaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override;

  /**
   * @brief Enqueues one newly ready task handle from any thread.
   *
   * @param handle Ready task handle to enqueue.
   * @param priority Scheduler priority.
   * @param epoch Optional epoch for lazy cancellation.
   * @return Nothing.
   * @throws std::bad_alloc if global queue growth fails before publication.
   * @note Stale epochs are rejected lazily at submission and dequeue without
   * scanning queues.
   */
  void submit_ready_task_handle_any_thread(
      TaskHandle handle, TaskPriority priority = TaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt);

  /**
   * @brief Enqueues a newly ready task-handle batch from any thread.
   *
   * @param handles Ready task handles to enqueue.
   * @param priority Scheduler priority shared by the batch.
   * @param epoch Optional epoch for lazy cancellation.
   * @return Nothing.
   * @throws std::bad_alloc if queue growth fails.
   * @note Both global priority queues use original-size rollback under one
   * lock; counters and notification publish only after every handle commits.
   */
  void submit_ready_task_handles_any_thread(
      std::vector<TaskHandle>&& handles,
      TaskPriority priority = TaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt);

  /**
   * @brief Waits for completion or consumes the batch's first exception.
   *
   * @return Nothing when the task count reaches zero or shutdown ends the
   * wait.
   * @throws The exact exception stored by the first publishing worker.
   * @note Success requires both the completion count and in-flight callback
   * count to reach zero. Failure requires queue cleanup to finish and every
   * callback from the captured batch epoch to settle before the exact pointer
   * is consumed under `exception_mutex_`. Therefore a caller may submit the
   * next batch immediately after this method returns without an old publisher
   * clearing, completing, or failing that new batch. Exception cleanup clears
   * the remaining count under `completion_count_mutex_` only if the captured
   * epoch remains active.
   */
  void wait_for_completion() override;

  /**
   * @brief Decrements the active batch's logical completion count.
   *
   * @return Nothing.
   * @throws Nothing under valid completion-mutex state.
   * @note Calls from a nonzero stale worker epoch and decrements at zero are
   * ignored. Reaching zero wakes the completion waiter, which separately
   * requires all dequeued callbacks to leave `in_flight_tasks_`.
   */
  void dec_tasks_to_complete() override;

  /**
   * @brief Adds logical work to the active scheduler batch.
   *
   * @param delta Positive number of logical tasks added by the caller.
   * @return Nothing.
   * @throws std::overflow_error if a current-epoch addition would exceed
   * `INT_MAX`.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note Nonpositive deltas and calls from a nonzero stale worker epoch are
   * ignored. Validation and mutation share the new-batch publication mutex,
   * so an old callback cannot pass validation and then change a newer batch.
   */
  void inc_tasks_to_complete(int delta) override;

  /**
   * @brief Publishes the first task exception for the current batch.
   *
   * @param e Non-null exception identity captured by a worker.
   * @return Nothing.
   * @throws Nothing under valid scheduler mutex state.
   * @note Null input is ignored. Otherwise, the worker epoch must match the
   * active batch. A separate claim atomic selects the first publisher; it
   * stores the protected pointer and epoch, rejects/dequeues further ready
   * work, drains all queues, marks cleanup complete, and only then
   * release-stores `has_exception_`. Duplicate or stale publishers cannot
   * overwrite the batch exception.
   */
  void set_exception(std::exception_ptr e) override;

  /**
   * @brief Copies one scheduler action into the attached host trace.
   * @param action Scheduler-facing action category.
   * @param node_id Graph node identifier associated with the action.
   * @return Nothing.
   * @throws Nothing.
   * @note A detached scheduler drops the event. Trace publication transfers no
   * graph or cache ownership and copies worker/epoch TLS metadata.
   */
  void log_event(SchedulerTraceAction action, int node_id) override;

  // ---------------------------------------------------------------------------
  // Epoch management
  // ---------------------------------------------------------------------------
  /**
   * @brief Reads the currently committed batch epoch.
   * @return Active epoch using acquire ordering.
   * @throws Nothing.
   * @note Failed transactional enqueue does not advance this value.
   */
  uint64_t active_epoch() const;

  /**
   * @brief Commits the next nonzero scheduler epoch.
   * @return Newly active nonzero epoch.
   * @throws Nothing.
   * @note The sequence wraps `UINT64_MAX` to one. Transactional batch paths
   * stage their epoch and publish it only after queue commit.
   */
  uint64_t begin_new_epoch();

  /**
   * @brief Checks whether a nonzero queued epoch differs from the active batch.
   * @param epoch Epoch carried by a scheduled callback or handle.
   * @return True for nonzero work outside the current epoch; false for epoch
   * zero or the current epoch.
   * @throws Nothing.
   * @note Cancellation drops scheduler work only and never mutates task graphs.
   */
  bool should_cancel_epoch(uint64_t epoch) const;

  /**
   * @brief Reads the epoch of the callback executing on this worker thread.
   * @return Worker TLS epoch, or zero outside scheduler callback execution.
   * @throws Nothing.
   * @note The value is diagnostic/request-local and owns no batch state.
   */
  static uint64_t this_task_epoch();

  /**
   * @brief Reads the current scheduler worker identifier.
   * @return Worker TLS id, or -1 outside a scheduler worker.
   * @throws Nothing.
   * @note The id indexes per-worker queues only while the scheduler is running.
   */
  static int this_worker_id();

 private:
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  /** @brief Grants the BUILD_TESTING consistency probe mutex-safe read access.
   */
  friend testing::SchedulerExceptionPublicationSnapshot
  testing::cpu_scheduler_exception_publication_snapshot(
      void* scheduler) noexcept;
  friend testing::SchedulerTransactionalStateSnapshot
  testing::cpu_scheduler_transactional_snapshot(void* scheduler) noexcept;
  friend void testing::set_cpu_scheduler_epoch_for_testing(
      void* scheduler, std::uint64_t epoch) noexcept;
#endif

  /**
   * @brief Queue-owned callback or borrowed dispatcher handle plus batch epoch.
   *
   * A scheduled entry owns a moved callback only when `use_handle==false`.
   * Handle entries borrow `TaskExecutor` through `TaskHandle` and therefore may
   * become worker-visible only after the enclosing batch commits completely.
   *
   * @return Movable scheduler queue value.
   * @throws std::bad_alloc only while constructing/moving callback storage.
   * @note Destruction never dereferences a borrowed executor and owns no graph,
   * task graph, cache, or runtime state.
   */
  struct ScheduledTask {
    /** @brief Batch epoch used for stale-work rejection and completion fencing.
     */
    uint64_t epoch{0};
    /** @brief Owned callback for closure-based submissions. */
    Task task;
    /** @brief Non-owning dispatcher executor/id tuple for handle submissions.
     */
    TaskHandle handle;
    /** @brief Selects borrowed handle execution instead of the owned callback.
     */
    bool use_handle{false};

    /**
     * @brief Constructs an empty queue value used during worker dequeue.
     * @return Empty scheduled task.
     * @throws Nothing.
     * @note The value owns and borrows nothing until move-assigned.
     */
    ScheduledTask() = default;

    /**
     * @brief Binds an owned callback to one batch epoch.
     * @param e Scheduler batch epoch.
     * @param t Callback whose ownership moves into this entry.
     * @return Callback-backed scheduled task.
     * @throws Nothing when `std::function` move construction is non-throwing.
     * @note The callback may capture request/cache state whose lifetime is
     * fenced by batch completion.
     */
    ScheduledTask(uint64_t e, Task&& t) : epoch(e), task(std::move(t)) {}

    /**
     * @brief Binds a borrowed dispatcher handle to one batch epoch.
     * @param e Scheduler batch epoch.
     * @param h Non-owning task handle copied into the queue entry.
     * @return Handle-backed scheduled task.
     * @throws Nothing.
     * @note The scheduler must not retain this entry after batch settlement.
     */
    ScheduledTask(uint64_t e, TaskHandle h)
        : epoch(e), handle(h), use_handle(true) {}

    /**
     * @brief Tests whether the selected callback/handle representation is
     * valid.
     * @return True for a callable task or valid borrowed handle.
     * @throws Nothing.
     * @note Validation does not dereference the handle's executor.
     */
    explicit operator bool() const {
      return use_handle ? static_cast<bool>(handle) : static_cast<bool>(task);
    }

    /**
     * @brief Invokes the selected callback or borrowed task handle.
     * @return Nothing.
     * @throws Any exception propagated by the callback or `TaskExecutor`.
     * @note Caller establishes worker TLS and in-flight fencing before entry.
     */
    void run() {
      if (use_handle) {
        handle.run();
      } else if (task) {
        task();
      }
    }
  };

  /**
   * @brief Executes the worker loop for one scheduler-owned CPU thread.
   *
   * The loop prefers high-priority global work, then local normal work, then
   * global normal work, and finally stolen work from peer queues. When no work
   * is visible, the worker parks on cv_task_available_ using
   * global_queues_mutex_. Every local enqueue updates ready_task_count_ while
   * holding both the global predicate mutex and its local queue mutex. Dequeue
   * decrements the count before releasing the owning queue mutex, while
   * exception cleanup holds the global mutex across every queue drain and the
   * final reset to zero. This prevents both lost wakeups and stale/negative
   * ready counts across failed-batch reuse.
   *
   * @param thread_id Stable worker index used for local queue ownership and
   * trace context.
   * @return Nothing until shutdown.
   * @throws Nothing escapes; task exceptions are captured with set_exception().
   * @note The method owns thread-local worker id and epoch context for the
   * lifetime of the worker thread.
   */
  void run_loop(int thread_id);

  /**
   * @brief Attempts to steal one normal-priority task from another worker.
   *
   * @param stealer_id Worker index that must not steal from its own queue.
   * @return Dequeued task, or `std::nullopt` when no peer work is available.
   * @throws Exceptions from thread-local random-device initialization or mutex
   * acquisition may propagate into the worker boundary.
   * @note A successful dequeue decrements the ready predicate and increments
   * the in-flight fence before releasing the victim queue mutex.
   */
  std::optional<ScheduledTask> steal_task(int stealer_id);

  /**
   * @brief Clears exception publication state before a new scheduler batch.
   *
   * @return Nothing.
   * @throws std::system_error only if locking the valid exception mutex fails.
   * @note Pointer, exception epoch, cleanup state, visible flag, and publisher
   * claim are reset under the exception lifecycle boundary. Nonempty initial
   * submissions call this only after every queue insertion succeeds and while
   * holding `global_queues_mutex_`, immediately before epoch/counter commit.
   * Start may call it while the scheduler is stopped.
   */
  void reset_exception_state();

  /**
   * @brief Marks one dequeued callback as fully settled.
   *
   * @return Nothing.
   * @throws Nothing under valid completion-mutex state.
   * @note Dequeue increments `in_flight_tasks_` while holding the corresponding
   * queue mutex. This method decrements it under `completion_mutex_` and wakes
   * waiters, so an exception batch cannot return while an old callback may
   * still publish completion or another exception.
   */
  void finish_in_flight_task() noexcept;

  // ---------------------------------------------------------------------------
  // Member state
  // ---------------------------------------------------------------------------
  /** @brief Borrowed host context; never owns graph or cache state. */
  SchedulerHostContext* host_context_ = nullptr;

  // Worker lifecycle
  /** @brief Scheduler-owned threads published only after complete start. */
  std::vector<std::thread> workers_;
  /** @brief Active worker/queue count, reset to zero on failed start/stop. */
  unsigned int num_workers_{0};
  /** @brief Immutable configured worker count used by each retry. */
  unsigned int configured_workers_{0};
  /** @brief Public lifecycle flag release-published after full worker install.
   */
  std::atomic<bool> running_{false};
  /** @brief Internal loop gate enabled while staged/committed workers may run.
   */
  std::atomic<bool> worker_loop_active_{false};

  // Per-worker normal-priority queues
  /** @brief Normal-priority LIFO/FIFO steal queues indexed by worker id. */
  std::vector<std::deque<ScheduledTask>> local_task_queues_;
  /** @brief Per-local-queue mutexes acquired after `global_queues_mutex_`. */
  std::vector<std::unique_ptr<std::mutex>> local_queue_mutexes_;

  // Global high- and normal-priority queues
  /** @brief Global high-priority FIFO queue with no partial batch visibility.
   */
  std::deque<ScheduledTask> high_priority_queue_;
  /** @brief Global normal-priority FIFO for submissions outside workers. */
  std::deque<ScheduledTask> normal_priority_queue_;
  /** @brief Root lock for global queues, local queues, and ready publication.
   */
  std::mutex global_queues_mutex_;
  /** @brief Worker notification paired with the global ready predicate. */
  std::condition_variable cv_task_available_;

  // Queue counters
  /** @brief Committed queued entries visible to worker wait predicates. */
  std::atomic<int> ready_task_count_{0};
  /** @brief Diagnostic count of CPU workers inside idle-wait handling. */
  std::atomic<int> sleeping_thread_count_{0};

  // Completion synchronization
  /**
   * @brief Serializes active-epoch/count publication with callback mutations.
   * @note Initial submissions acquire queue gates before this mutex. Counter
   *       mutations acquire only this mutex, preventing cross-epoch writes.
   */
  std::mutex completion_count_mutex_;
  /** @brief Serializes completion waits and in-flight settlement notification.
   */
  std::mutex completion_mutex_;
  /** @brief Wakes waiters after logical/in-flight completion or failure. */
  std::condition_variable cv_completion_;
  /** @brief Logical active-batch completions published only after queue commit.
   */
  std::atomic<int> tasks_to_complete_{0};
  /** @brief Dequeued callbacks not yet returned to their worker loop. */
  std::atomic<int> in_flight_tasks_{0};

  // Exception transport
  /** @brief Protects the exact exception pointer during publish/consume. */
  std::mutex exception_mutex_;
  /** @brief First exact worker exception retained for the active batch. */
  std::exception_ptr first_exception_{nullptr};
  /** @brief First-publisher latch retained until the next batch reset. */
  std::atomic<bool> exception_claimed_{false};
  /** @brief Acquire/release visibility flag published only after the pointer.
   */
  std::atomic<bool> has_exception_{false};
  /** @brief Batch epoch paired with `first_exception_`. */
  std::atomic<uint64_t> exception_epoch_{0};
  /** @brief True only after claimed-batch queues have been drained. */
  std::atomic<bool> exception_cleanup_complete_{false};

  // Epoch management
  /** @brief Last committed nonzero epoch; the sequence wraps maximum to one. */
  std::atomic<uint64_t> epoch_counter_{0};
  /** @brief Epoch used by stale-work checks and exception fencing. */
  std::atomic<uint64_t> active_epoch_{0};
  /** @brief Current worker id or -1 outside scheduler-owned threads. */
  static thread_local int tls_worker_id_;
  /** @brief Epoch of the callback currently running on this thread. */
  static thread_local uint64_t tls_active_epoch_;

  // Statistics
  /** @brief Successfully committed high-priority entries. */
  std::atomic<uint64_t> high_enqueued_{0};
  /** @brief Successfully committed normal-priority entries. */
  std::atomic<uint64_t> normal_enqueued_{0};
  /** @brief High-priority callbacks dequeued for execution. */
  std::atomic<uint64_t> high_executed_{0};
  /** @brief Normal-priority callbacks dequeued for execution. */
  std::atomic<uint64_t> normal_executed_{0};
  /** @brief Aggregate diagnostic scheduling count retained for stats. */
  std::atomic<uint64_t> total_tasks_scheduled_{0};
};

}  // namespace ps
