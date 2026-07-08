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

namespace ps {

class GraphRuntime;
// =============================================================================
// CpuWorkStealingScheduler: CPU Work-Stealing 调度器
// 实现基于工作窃取的多线程任务调度，从 GraphRuntime 迁移而来
// =============================================================================
class CpuWorkStealingScheduler : public IScheduler,
                                 public SchedulerTaskRuntime {
 public:
  /// @brief 构造函数
  /// @param num_workers 工作线程数量，0 表示使用硬件并发数
  explicit CpuWorkStealingScheduler(unsigned int num_workers = 0);
  ~CpuWorkStealingScheduler() override;

  // 禁用拷贝
  CpuWorkStealingScheduler(const CpuWorkStealingScheduler&) = delete;
  CpuWorkStealingScheduler& operator=(const CpuWorkStealingScheduler&) = delete;

  // ---------------------------------------------------------------------------
  // IScheduler 接口实现
  // ---------------------------------------------------------------------------
  void attach(GraphRuntime* runtime) override;
  void detach() override;
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
   * @throws Nothing directly; std::thread::join may terminate only if the
   * thread object is invalid, which the scheduler guards with joinable().
   * @note Pending queued callbacks are discarded after all workers have
   * observed the stop state and exited.
   */
  void shutdown() override;
  std::string name() const override;
  std::string get_stats() const override;
  bool is_running() const override;
  bool task_runtime_running() const override;

  // ---------------------------------------------------------------------------
  // 调度器内部任务提交 API（供 ComputeService 使用）
  // ---------------------------------------------------------------------------
  using Task = SchedulerTaskRuntime::Task;
  using TaskPriority = SchedulerTaskPriority;

  /**
   * @brief 提交初始任务集合并开始一个新的调度 epoch。
   *
   * 该入口重置批次异常状态，递增 active epoch，设置
   * tasks_to_complete_，然后按优先级发布初始 ready work。高优先级任务进入全局
   * FIFO 队列；普通优先级任务按 worker 分散到本地队列，并通过
   * publish_ready_tasks() 在全局等待互斥量下发布 ready 计数，避免单 worker
   * 已准备睡眠时错过本地队列任务唤醒。
   *
   * @param tasks 本批次立即 ready 的回调任务；scheduler 接管其可调用对象。
   * @param total_task_count
   * 本批次需要完成的总任务数，包括任务运行期间动态增加的 ready work。
   * @param priority 初始任务的调度优先级。
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
   * epoch、异常重置、完成计数和唤醒语义。高优先级句柄进入 全局 FIFO
   * 队列；普通句柄进入 per-worker 本地队列，并在所有有效句柄入队后通过
   * publish_ready_tasks() 发布 ready 数量。这样本地队列可见性与 worker
   * condition-variable 睡眠判定保持同一个唤醒边界。
   *
   * @param handles 调度器借用的轻量任务句柄列表；空句柄会被跳过。
   * @param total_task_count 本批次需要完成的活跃任务数。
   * @param priority 任务优先级。
   * @throws Nothing directly; queue allocation may throw before enqueue.
   * @note 句柄保持 dispatcher-owned executor 指针，scheduler 不拥有 task
   * graph。
   */
  void submit_initial_task_handles(
      std::vector<TaskHandle>&& handles, int total_task_count,
      TaskPriority priority = TaskPriority::Normal) override;

  /// @brief 从工作线程内部提交新就绪的任务
  void submit_ready_task_from_worker(
      Task&& task, TaskPriority priority = TaskPriority::Normal) override;

  /**
   * @brief 从工作线程内部提交一个新就绪任务句柄。
   *
   * @param handle 依赖计数归零后释放的任务句柄。
   * @param priority 任务优先级。
   * @throws Nothing directly.
   * @note 普通优先级会优先进入当前 worker 的本地队列。
   */
  void submit_ready_task_handle_from_worker(
      TaskHandle handle, TaskPriority priority = TaskPriority::Normal) override;

  /**
   * @brief 从工作线程内部批量提交新就绪任务句柄。
   *
   * @param handles 同一依赖释放阶段产生的 ready 句柄。
   * @param priority 任务优先级。
   * @throws std::bad_alloc if queue growth fails.
   * @note 批量路径减少 tile 级调度时的逐任务锁和唤醒。
   */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      TaskPriority priority = TaskPriority::Normal) override;

  /// @brief 从任意线程提交新就绪的任务
  void submit_ready_task_any_thread(
      Task&& task, TaskPriority priority = TaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override;

  /**
   * @brief 从任意线程提交一个新就绪任务句柄。
   *
   * @param handle Ready task handle to enqueue.
   * @param priority Scheduler priority.
   * @param epoch Optional epoch for lazy cancellation.
   * @throws Nothing directly.
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
   * @throws std::bad_alloc if queue growth fails.
   * @note 高优先级批次共享一次全局队列锁和一次通知。
   */
  void submit_ready_task_handles_any_thread(
      std::vector<TaskHandle>&& handles,
      TaskPriority priority = TaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override;

  /// @brief 等待当前批次的所有任务完成
  void wait_for_completion() override;

  /// @brief 减少待完成任务计数
  void dec_tasks_to_complete() override;

  /// @brief 增加待完成任务计数
  void inc_tasks_to_complete(int delta) override;

  /// @brief 设置异常状态
  void set_exception(std::exception_ptr e) override;

  void log_event(SchedulerTraceAction action, int node_id) override;

  // ---------------------------------------------------------------------------
  // Epoch 管理
  // ---------------------------------------------------------------------------
  uint64_t active_epoch() const;
  uint64_t begin_new_epoch();
  bool should_cancel_epoch(uint64_t epoch) const;
  static uint64_t this_task_epoch();
  static int this_worker_id();

 private:
  // 内部任务结构
  struct ScheduledTask {
    uint64_t epoch{0};
    Task task;
    TaskHandle handle;
    bool use_handle{false};

    ScheduledTask() = default;
    ScheduledTask(uint64_t e, Task&& t) : epoch(e), task(std::move(t)) {}
    ScheduledTask(uint64_t e, TaskHandle h)
        : epoch(e), handle(h), use_handle(true) {}
    explicit operator bool() const {
      return use_handle ? static_cast<bool>(handle) : static_cast<bool>(task);
    }
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
   * global_queues_mutex_. All local-queue publishers must update
   * ready_task_count_ through publish_ready_tasks() under that same mutex so a
   * single worker cannot miss the only wakeup for ordinary initial work.
   *
   * @param thread_id Stable worker index used for local queue ownership and
   * trace context.
   * @throws Nothing escapes; task exceptions are captured with set_exception().
   * @note The method owns thread-local worker id and epoch context for the
   * lifetime of the worker thread.
   */
  void run_loop(int thread_id);

  // 从其他工作线程窃取任务
  std::optional<ScheduledTask> steal_task(int stealer_id);

  /**
   * @brief Publishes newly visible local-queue work to idle workers.
   *
   * Local normal-priority queues have per-worker mutexes, but idle workers
   * decide whether to sleep under global_queues_mutex_ by reading
   * ready_task_count_. This helper updates that predicate while holding the
   * global mutex, then wakes one or all workers. The ordering closes the
   * condition-variable lost-wakeup window that matters most when there is only
   * one worker and no later task submission can send another notification.
   *
   * @param count Number of ready tasks made available by the caller.
   * @param wake_all true to wake all workers for a batch, false to wake one
   * worker for a single local submission.
   * @throws Nothing directly.
   * @note Call after the tasks have been placed in their local queues. If an
   * already-running worker consumes a task before publication, the later
   * increment balances that worker's ready_task_count_ decrement.
   */
  void publish_ready_tasks(int count, bool wake_all);

  // ---------------------------------------------------------------------------
  // 成员变量
  // ---------------------------------------------------------------------------
  GraphRuntime* runtime_ = nullptr;

  // 工作线程管理
  std::vector<std::thread> workers_;
  unsigned int num_workers_{0};
  unsigned int configured_workers_{0};
  std::atomic<bool> running_{false};

  // 本地任务队列（per-worker，用于普通优先级任务）
  std::vector<std::deque<ScheduledTask>> local_task_queues_;
  std::vector<std::unique_ptr<std::mutex>> local_queue_mutexes_;

  // 全局任务队列（高优先级 + 普通优先级）
  std::queue<ScheduledTask> high_priority_queue_;
  std::queue<ScheduledTask> normal_priority_queue_;
  std::mutex global_queues_mutex_;
  std::condition_variable cv_task_available_;

  // 任务计数器
  std::atomic<int> ready_task_count_{0};
  std::atomic<int> sleeping_thread_count_{0};

  // 完成同步
  std::mutex completion_mutex_;
  std::condition_variable cv_completion_;
  std::atomic<int> tasks_to_complete_{0};

  // 异常处理
  std::mutex exception_mutex_;
  std::exception_ptr first_exception_{nullptr};
  std::atomic<bool> has_exception_{false};

  // Epoch 管理
  std::atomic<uint64_t> epoch_counter_{0};
  std::atomic<uint64_t> active_epoch_{0};
  static thread_local int tls_worker_id_;
  static thread_local uint64_t tls_active_epoch_;

  // 统计信息
  std::atomic<uint64_t> high_enqueued_{0};
  std::atomic<uint64_t> normal_enqueued_{0};
  std::atomic<uint64_t> high_executed_{0};
  std::atomic<uint64_t> normal_executed_{0};
  std::atomic<uint64_t> total_tasks_scheduled_{0};
};

}  // namespace ps
