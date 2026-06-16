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

  /// @brief 提交初始任务集合，开始一次计算批次
  /// @param tasks 任务列表
  /// @param total_task_count 总任务数（包括后续可能动态添加的任务）
  /// @param priority 任务优先级
  void submit_initial_tasks(
      std::vector<Task>&& tasks, int total_task_count,
      TaskPriority priority = TaskPriority::Normal) override;

  /**
   * @brief 提交初始任务句柄集合，开始一次计算批次。
   *
   * @param handles 调度器借用的轻量任务句柄列表。
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

  // 工作线程主循环
  void run_loop(int thread_id);

  // 从其他工作线程窃取任务
  std::optional<ScheduledTask> steal_task(int stealer_id);

  // 取消过期的排队任务
  void cancel_stale_enqueued_tasks(uint64_t min_epoch);

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
