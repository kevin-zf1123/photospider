// Photospider kernel: CpuWorkStealingScheduler
// M3.3: 将现有的 run_loop 和队列逻辑迁移至可插拔调度器
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

#include "kernel/scheduler/i_scheduler.hpp"
#include "kernel/scheduler/scheduler_task_runtime.hpp"

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
#include "scheduler/scheduler_exception_test_hooks.hpp"
#endif

namespace ps {

class GraphRuntime;
/**
 * @brief Dispatches ready callbacks through priority and work-stealing queues.
 *
 * The scheduler owns worker threads, global high/normal queues, per-worker
 * normal queues, batch epoch/counter state, and first-exception publication.
 * It does not own `GraphRuntime`, task graphs, cache state, or executors behind
 * borrowed `TaskHandle` values. Batch handle publication is transactional:
 * workers cannot observe any handle until every queue insertion commits.
 *
 * @return Concrete `IScheduler` and `SchedulerTaskRuntime` implementation.
 * @throws std::bad_alloc from explicit construction/start/submission methods.
 * @throws std::system_error from explicit worker lifecycle operations.
 * @note Lifecycle calls are externally serialized. A borrowed handle remains
 * valid only through the matching `wait_for_completion()` batch boundary.
 */
class CpuWorkStealingScheduler : public IScheduler,
                                 public SchedulerTaskRuntime {
 public:
  /**
   * @brief Configures a stopped CPU work-stealing scheduler.
   * @param num_workers Worker count, or zero to use hardware concurrency.
   * @return A stopped scheduler that owns no worker threads yet.
   * @throws Nothing directly; hardware concurrency failure falls back to one.
   * @note The runtime pointer remains null until `attach()`; no queue, cache,
   * or task-executor ownership is acquired by construction.
   */
  explicit CpuWorkStealingScheduler(unsigned int num_workers = 0);

  /**
   * @brief Stops and releases scheduler-owned worker resources.
   * @return Nothing.
   * @throws Nothing by destructor contract; valid built-in shutdown is expected
   * not to fail while joining scheduler-owned threads.
   * @note Borrowed runtime/task-handle targets must already satisfy their
   * lifecycle contracts when destruction begins.
   */
  ~CpuWorkStealingScheduler() override;

  // 禁用拷贝
  CpuWorkStealingScheduler(const CpuWorkStealingScheduler&) = delete;
  CpuWorkStealingScheduler& operator=(const CpuWorkStealingScheduler&) = delete;

  // ---------------------------------------------------------------------------
  // IScheduler 接口实现
  // ---------------------------------------------------------------------------
  /**
   * @brief Attaches a borrowed graph runtime for scheduler trace publication.
   * @param runtime Runtime that outlives the attachment, or nullptr in tests.
   * @return Nothing.
   * @throws Nothing.
   * @note Attachment transfers no GraphModel, cache, scheduler, or runtime
   * ownership and must occur before start under GraphRuntime serialization.
   */
  void attach(GraphRuntime* runtime) override;

  /**
   * @brief Clears the borrowed runtime trace target.
   * @return Nothing.
   * @throws Nothing.
   * @note Detach does not stop workers; GraphRuntime orders shutdown first.
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

  /**
   * @brief Reports scheduler task-runtime availability.
   * @return Same lifecycle value as `is_running()`.
   * @throws Nothing.
   * @note This does not imply a task batch is active or cache state is ready.
   */
  bool task_runtime_running() const override;

  // ---------------------------------------------------------------------------
  // 调度器内部任务提交 API（供 ComputeService 使用）
  // ---------------------------------------------------------------------------
  using Task = SchedulerTaskRuntime::Task;
  using TaskPriority = SchedulerTaskPriority;

  /**
   * @brief 提交初始任务集合并开始一个新的调度 epoch。
   *
   * The method stages the next epoch, inserts the complete batch while holding
   * the global visibility lock, and rolls every touched deque back to its
   * original size if any insertion throws. Only a complete queue commit
   * publishes epoch, logical/ready counters, statistics, and notification.
   *
   * @param tasks 本批次立即 ready 的回调任务；scheduler 接管其可调用对象。
   * @param total_task_count
   * 本批次需要完成的总任务数，包括任务运行期间动态增加的 ready work。
   * @param priority 初始任务的调度优先级。
   * @return Nothing.
   * @throws std::bad_alloc if queue growth fails while storing callbacks.
   * @note 调用方必须保证 total_task_count 与任务内部的 dec_tasks_to_complete()
   * 调用保持一致；空批次会直接通知 completion waiter。
   */
  void submit_initial_tasks(
      std::vector<Task>&& tasks, int total_task_count,
      TaskPriority priority = TaskPriority::Normal) override;

  /**
   * @brief 提交初始任务句柄集合，开始一次计算批次。
   *
   * 句柄路径与回调路径共享
   * epoch、异常重置、完成计数和唤醒语义。高优先级句柄进入全局 FIFO；普通句柄
   * 分布到 per-worker 队列。所有本地读写都使用 global→local 锁序，因此整个批次
   * 可在异常时无分配回退，且任何借用句柄只在 epoch/counter 完整提交后可见。
   *
   * @param handles 调度器借用的轻量任务句柄列表；空句柄会被跳过。
   * @param total_task_count 本批次需要完成的活跃任务数。
   * @param priority 任务优先级。
   * @return Nothing.
   * @throws std::bad_alloc if queue growth fails while storing handles.
   * @note 句柄保持 dispatcher-owned executor 指针，scheduler 不拥有 task
   * graph。
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
      Task&& task, TaskPriority priority = TaskPriority::Normal) override;

  /**
   * @brief 从工作线程内部提交一个新就绪任务句柄。
   *
   * @param handle 依赖计数归零后释放的任务句柄。
   * @param priority 任务优先级。
   * @return Nothing.
   * @throws std::bad_alloc if queue growth fails before publication.
   * @note 普通优先级会优先进入当前 worker 的本地队列，并使用
   * global→local 锁序与 ready predicate 共同发布。
   */
  void submit_ready_task_handle_from_worker(
      TaskHandle handle, TaskPriority priority = TaskPriority::Normal) override;

  /**
   * @brief 从工作线程内部批量提交新就绪任务句柄。
   *
   * @param handles 同一依赖释放阶段产生的 ready 句柄。
   * @param priority 任务优先级。
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
   * @brief 从任意线程提交一个新就绪任务句柄。
   *
   * @param handle Ready task handle to enqueue.
   * @param priority Scheduler priority.
   * @param epoch Optional epoch for lazy cancellation.
   * @return Nothing.
   * @throws std::bad_alloc if global queue growth fails before publication.
   * @note 旧 epoch 在提交和出队时惰性丢弃，不扫描队列。
   */
  void submit_ready_task_handle_any_thread(
      TaskHandle handle, TaskPriority priority = TaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override;

  /**
   * @brief 从任意线程批量提交新就绪任务句柄。
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
      std::optional<uint64_t> epoch = std::nullopt) override;

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
   * clearing, completing, or failing that new batch.
   */
  void wait_for_completion() override;

  /**
   * @brief Decrements the active batch's logical completion count.
   *
   * @return Nothing.
   * @throws Nothing under valid completion-mutex state.
   * @note Calls from a nonzero stale worker epoch are ignored. Reaching zero
   * wakes the completion waiter, which separately requires all dequeued
   * callbacks to leave `in_flight_tasks_`.
   */
  void dec_tasks_to_complete() override;

  /**
   * @brief Adds logical work to the active scheduler batch.
   *
   * @param delta Positive number of logical tasks added by the caller.
   * @return Nothing.
   * @throws Nothing.
   * @note Nonpositive deltas and calls from a nonzero stale worker epoch are
   * ignored so an old callback cannot change a newer batch.
   */
  void inc_tasks_to_complete(int delta) override;

  /**
   * @brief Publishes the first task exception for the current batch.
   *
   * @param e Non-null exception identity captured by a worker.
   * @return Nothing.
   * @throws Nothing under valid scheduler mutex state.
   * @note The worker epoch must match the active batch. A separate claim atomic
   * selects the first publisher; it stores the protected pointer and epoch,
   * rejects/dequeues further ready work, drains all queues, marks cleanup
   * complete, and only then release-stores `has_exception_`. Duplicate or stale
   * publishers cannot overwrite the batch exception.
   */
  void set_exception(std::exception_ptr e) override;

  /**
   * @brief Copies one scheduler action into the attached runtime trace.
   * @param action Scheduler-facing action category.
   * @param node_id Graph node identifier associated with the action.
   * @return Nothing.
   * @throws std::bad_alloc if runtime trace storage grows.
   * @note A null runtime drops the event. Trace publication transfers no graph
   * or cache ownership and uses worker/epoch TLS only as copied metadata.
   */
  void log_event(SchedulerTraceAction action, int node_id) override;

  // ---------------------------------------------------------------------------
  // Epoch 管理
  // ---------------------------------------------------------------------------
  /**
   * @brief Reads the currently committed batch epoch.
   * @return Active epoch using acquire ordering.
   * @throws Nothing.
   * @note Failed transactional enqueue does not advance this value.
   */
  uint64_t active_epoch() const;

  /**
   * @brief Commits the next monotonically increasing scheduler epoch.
   * @return Newly active nonzero epoch.
   * @throws Nothing.
   * @note Transactional batch paths stage their epoch and publish it only after
   * queue commit; this helper is used for allocation-free empty batches.
   */
  uint64_t begin_new_epoch();

  /**
   * @brief Checks whether a nonzero queued epoch precedes the active batch.
   * @param epoch Epoch carried by a scheduled callback or handle.
   * @return True for stale nonzero work; false for epoch zero/current/future.
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
  // 成员变量
  // ---------------------------------------------------------------------------
  /** @brief Borrowed trace runtime; never owns GraphModel or cache state. */
  GraphRuntime* runtime_ = nullptr;

  // 工作线程管理
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

  // 本地任务队列（per-worker，用于普通优先级任务）
  /** @brief Normal-priority LIFO/FIFO steal queues indexed by worker id. */
  std::vector<std::deque<ScheduledTask>> local_task_queues_;
  /** @brief Per-local-queue mutexes acquired after `global_queues_mutex_`. */
  std::vector<std::unique_ptr<std::mutex>> local_queue_mutexes_;

  // 全局任务队列（高优先级 + 普通优先级）
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

  // 任务计数器
  /** @brief Committed queued entries visible to worker wait predicates. */
  std::atomic<int> ready_task_count_{0};
  /** @brief Diagnostic count of CPU workers inside idle-wait handling. */
  std::atomic<int> sleeping_thread_count_{0};

  // 完成同步
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

  // 异常处理
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

  // Epoch 管理
  /** @brief Last committed epoch number; failed batch staging does not advance.
   */
  std::atomic<uint64_t> epoch_counter_{0};
  /** @brief Epoch used by stale-work checks and exception fencing. */
  std::atomic<uint64_t> active_epoch_{0};
  /** @brief Current worker id or -1 outside scheduler-owned threads. */
  static thread_local int tls_worker_id_;
  /** @brief Epoch of the callback currently running on this thread. */
  static thread_local uint64_t tls_active_epoch_;

  // 统计信息
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
