// Photospider kernel: GpuPipelineScheduler
// M3.5: 异构调度器 - 支持 HP 走 GPU、RT 走 CPU 的混合计算模式
// Scheduler dispatches already-planned HP/RT tasks.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "kernel/scheduler/i_scheduler.hpp"
#include "kernel/scheduler/scheduler_task_runtime.hpp"

namespace ps {

class GraphRuntime;

// =============================================================================
// GpuPipelineScheduler: GPU Pipeline 调度器
// 实现异构队列调度：HP 可进入 GPU 队列，RT 使用高优先级 CPU 队列
// 支持 RT 和 HP 同时调度，其中 RT 优先级高于 HP
// =============================================================================
class GpuPipelineScheduler : public IScheduler, public SchedulerTaskRuntime {
 public:
  /// @brief 调度配置
  struct Config {
    // GPU 工作线程数（用于 Metal 命令提交）
    unsigned int gpu_workers;
    // CPU 工作线程数（用于 RT 和 CPU 回退）
    unsigned int cpu_workers;  // 0 表示使用硬件并发数
    // 是否优先使用 GPU（HP 模式）
    bool prefer_gpu_for_hp;

    Config() : gpu_workers(1), cpu_workers(0), prefer_gpu_for_hp(true) {}
  };

  /// @brief 构造函数
  /// @param config 调度配置
  explicit GpuPipelineScheduler(const Config& config = Config());
  ~GpuPipelineScheduler() override;

  // 禁用拷贝
  GpuPipelineScheduler(const GpuPipelineScheduler&) = delete;
  GpuPipelineScheduler& operator=(const GpuPipelineScheduler&) = delete;

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
  // 调度器内部 API
  // ---------------------------------------------------------------------------
  using Task = SchedulerTaskRuntime::Task;
  using TaskPriority = SchedulerTaskPriority;

  /// @brief 提交任务到 RT 队列（高优先级）
  void submit_rt_task(Task&& task, uint64_t epoch = 0);

  /**
   * @brief 提交 RT 任务句柄到高优先级 CPU 队列。
   *
   * @param handle Dispatcher-owned ready task handle.
   * @param epoch Scheduler epoch used for lazy cancellation.
   * @throws Nothing directly.
   * @note 句柄路径避免在 tile 级 ready work 上为队列项分配闭包。
   */
  void submit_rt_task_handle(TaskHandle handle, uint64_t epoch = 0);

  /// @brief 提交任务到 HP 队列（普通优先级）
  void submit_hp_task(Task&& task, uint64_t epoch = 0);

  /**
   * @brief 提交 HP CPU 任务句柄到普通优先级队列。
   *
   * @param handle Dispatcher-owned ready task handle.
   * @param epoch Scheduler epoch used for lazy cancellation.
   * @throws Nothing directly.
   */
  void submit_hp_task_handle(TaskHandle handle, uint64_t epoch = 0);

  /// @brief 提交 GPU 任务
  void submit_gpu_task(Task&& task, uint64_t epoch = 0);

  /**
   * @brief 提交 GPU 任务句柄。
   *
   * @param handle Dispatcher-owned ready task handle.
   * @param epoch Scheduler epoch used for lazy cancellation.
   * @throws Nothing directly.
   */
  void submit_gpu_task_handle(TaskHandle handle, uint64_t epoch = 0);

  /// @brief 等待当前批次完成
  void wait_for_completion() override;

  /// @brief 减少待完成任务计数
  void dec_tasks_to_complete() override;

  /// @brief 增加待完成任务计数
  void inc_tasks_to_complete(int delta) override;

  /// @brief 设置异常状态
  void set_exception(std::exception_ptr e) override;

  void submit_initial_tasks(
      std::vector<Task>&& tasks, int total_task_count,
      TaskPriority priority = TaskPriority::Normal) override;

  void submit_initial_task_handles(
      std::vector<TaskHandle>&& handles, int total_task_count,
      TaskPriority priority = TaskPriority::Normal) override;

  void submit_ready_task_from_worker(
      Task&& task, TaskPriority priority = TaskPriority::Normal) override;

  void submit_ready_task_handle_from_worker(
      TaskHandle handle, TaskPriority priority = TaskPriority::Normal) override;

  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      TaskPriority priority = TaskPriority::Normal) override;

  void submit_ready_task_any_thread(
      Task&& task, TaskPriority priority = TaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override;

  void submit_ready_task_handle_any_thread(
      TaskHandle handle, TaskPriority priority = TaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override;

  void submit_ready_task_handles_any_thread(
      std::vector<TaskHandle>&& handles,
      TaskPriority priority = TaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override;

  void log_event(SchedulerTraceAction action, int node_id) override;

  // ---------------------------------------------------------------------------
  // Epoch 管理
  // ---------------------------------------------------------------------------
  uint64_t active_epoch() const;
  uint64_t begin_new_epoch();
  bool should_cancel_epoch(uint64_t epoch) const;
  static uint64_t this_task_epoch();
  static int this_worker_id();

  // ---------------------------------------------------------------------------
  // 设备能力查询
  // ---------------------------------------------------------------------------

  /// @brief 检查 GPU 是否可用
  bool is_gpu_available() const;

  /**
   * @brief Returns devices that HP production compute may target here.
   *
   * @return CPU plus GPU_METAL only when a Metal device is attached and HP GPU
   * dispatch is enabled by config and worker availability.
   * @throws std::bad_alloc if vector allocation fails.
   * @note This list feeds TaskSubmissionPlan operation resolution. It must
   * remain aligned with can_dispatch_hp_to_gpu() so disabled GPU workers or
   * CPU-for-HP configurations do not select GPU implementations for CPU queues.
   */
  std::vector<Device> get_available_devices() const;

  /**
   * @brief Reports devices available to compute tasks submitted here.
   *
   * @return Same list as get_available_devices().
   * @throws std::bad_alloc if vector allocation fails.
   * @note This overrides SchedulerTaskRuntime so production operation
   * resolution can choose registered per-device implementations only for
   * devices this scheduler is currently willing to dispatch.
   */
  std::vector<Device> available_devices() const override;

 private:
  // 内部任务结构
  struct ScheduledTask {
    uint64_t epoch{0};
    Task task;
    TaskHandle handle;
    bool use_handle{false};
    ComputeIntent intent{ComputeIntent::GlobalHighPrecision};

    ScheduledTask() = default;
    ScheduledTask(uint64_t e, Task&& t,
                  ComputeIntent i = ComputeIntent::GlobalHighPrecision)
        : epoch(e), task(std::move(t)), intent(i) {}
    ScheduledTask(uint64_t e, TaskHandle h,
                  ComputeIntent i = ComputeIntent::GlobalHighPrecision)
        : epoch(e), handle(h), use_handle(true), intent(i) {}
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

  // CPU 工作线程主循环
  void cpu_run_loop(int thread_id);

  // GPU 工作线程主循环
  void gpu_run_loop(int thread_id);

  // 在 runtime attach 后按需启动 GPU worker。
  void start_gpu_workers_if_available();

  // HP 任务是否可以安全进入 GPU 队列。
  bool can_dispatch_hp_to_gpu() const;

  // 从其他工作线程窃取任务
  std::optional<ScheduledTask> steal_task(int stealer_id);

  // ---------------------------------------------------------------------------
  // 成员变量
  // ---------------------------------------------------------------------------
  GraphRuntime* runtime_ = nullptr;
  Config config_;

  // CPU 工作线程
  std::vector<std::thread> cpu_workers_;
  unsigned int num_cpu_workers_{0};

  // GPU 工作线程
  std::vector<std::thread> gpu_workers_;
  unsigned int num_gpu_workers_{0};

  std::atomic<bool> running_{false};

  // RT 任务队列（高优先级，CPU 处理）
  std::queue<ScheduledTask> rt_queue_;
  std::mutex rt_queue_mutex_;
  std::condition_variable rt_cv_;

  // HP CPU 任务队列（普通优先级）
  std::queue<ScheduledTask> hp_cpu_queue_;
  std::mutex hp_cpu_queue_mutex_;
  std::condition_variable hp_cpu_cv_;

  // GPU 任务队列（HP 优先使用）
  std::queue<ScheduledTask> gpu_queue_;
  std::mutex gpu_queue_mutex_;
  std::condition_variable gpu_cv_;

  // 任务计数器
  std::atomic<int> rt_ready_count_{0};
  std::atomic<int> hp_cpu_ready_count_{0};
  std::atomic<int> gpu_ready_count_{0};
  std::atomic<int> sleeping_cpu_count_{0};
  std::atomic<int> sleeping_gpu_count_{0};

  // 完成同步
  std::mutex completion_mutex_;
  std::condition_variable cv_completion_;
  std::atomic<int> tasks_to_complete_{0};

  // Epoch 管理
  std::atomic<uint64_t> epoch_counter_{0};
  std::atomic<uint64_t> active_epoch_{0};

  // 异常处理
  std::mutex exception_mutex_;
  std::exception_ptr first_exception_;
  std::atomic<bool> has_exception_{false};

  // 统计信息
  std::atomic<uint64_t> rt_tasks_executed_{0};
  std::atomic<uint64_t> hp_cpu_tasks_executed_{0};
  std::atomic<uint64_t> gpu_tasks_executed_{0};
  std::atomic<uint64_t> total_tasks_scheduled_{0};

  // Thread-local storage
  static thread_local int tls_worker_id_;
  static thread_local uint64_t tls_active_epoch_;
  static thread_local bool tls_is_gpu_worker_;
};

}  // namespace ps
