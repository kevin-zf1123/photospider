// Photospider kernel: GpuPipelineScheduler
// M3.5: 异构调度器 - 支持 HP 走 GPU、RT 走 CPU 的混合计算模式
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "kernel/scheduler/i_scheduler.hpp"

namespace ps {

class GraphRuntime;
class GraphTraversalService;
class GraphCacheService;
class ComputeService;

// =============================================================================
// GpuPipelineScheduler: GPU Pipeline 调度器
// 实现异构调度：HP 优先使用 GPU，RT 优先使用 CPU 以保证低延迟
// 支持 RT 和 HP 同时调度，其中 RT 优先级高于 HP
// =============================================================================
class GpuPipelineScheduler : public IScheduler {
 public:
  /// @brief 调度配置
  struct Config {
    // GPU 工作线程数（用于 Metal 命令提交）
    unsigned int gpu_workers;
    // CPU 工作线程数（用于 RT 和 CPU 回退）
    unsigned int cpu_workers;  // 0 表示使用硬件并发数
    // 是否优先使用 GPU（HP 模式）
    bool prefer_gpu_for_hp;
    // 是否强制 RT 使用 CPU（保证低延迟）
    bool force_cpu_for_rt;
    // RT 任务抢占阈值（毫秒）
    int rt_preempt_threshold_ms;
    
    Config()
        : gpu_workers(1),
          cpu_workers(0),
          prefer_gpu_for_hp(true),
          force_cpu_for_rt(true),
          rt_preempt_threshold_ms(16) {}
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
  std::future<NodeOutput> schedule(const ComputeOptions& opts) override;
  std::string name() const override;
  std::string get_stats() const override;
  bool is_running() const override;

  // ---------------------------------------------------------------------------
  // 调度器内部 API
  // ---------------------------------------------------------------------------
  using Task = std::function<void()>;
  enum class TaskPriority { Normal, High };

  /// @brief 提交任务到 RT 队列（高优先级）
  void submit_rt_task(Task&& task, uint64_t epoch = 0);

  /// @brief 提交任务到 HP 队列（普通优先级）
  void submit_hp_task(Task&& task, uint64_t epoch = 0);

  /// @brief 提交 GPU 任务
  void submit_gpu_task(Task&& task, uint64_t epoch = 0);

  /// @brief 等待当前批次完成
  void wait_for_completion();

  /// @brief 减少待完成任务计数
  void dec_tasks_to_complete();

  /// @brief 增加待完成任务计数
  void inc_tasks_to_complete(int delta);

  /// @brief 设置异常状态
  void set_exception(std::exception_ptr e);

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
  
  /// @brief 获取当前可用设备列表
  std::vector<Device> get_available_devices() const;

 private:
  // 内部任务结构
  struct ScheduledTask {
    uint64_t epoch{0};
    Task task;
    ComputeIntent intent{ComputeIntent::GlobalHighPrecision};

    ScheduledTask() = default;
    ScheduledTask(uint64_t e, Task&& t, ComputeIntent i = ComputeIntent::GlobalHighPrecision)
        : epoch(e), task(std::move(t)), intent(i) {}
    explicit operator bool() const { return static_cast<bool>(task); }
  };

  // CPU 工作线程主循环
  void cpu_run_loop(int thread_id);
  
  // GPU 工作线程主循环
  void gpu_run_loop(int thread_id);
  
  // 从其他工作线程窃取任务
  std::optional<ScheduledTask> steal_task(int stealer_id);
  
  // 取消过期的排队任务
  void cancel_stale_enqueued_tasks(uint64_t min_epoch);

  // 执行单次计算
  NodeOutput execute_compute(const ComputeOptions& opts);
  
  // 选择最优算子实现
  const OpImplementation* select_implementation(
      const std::string& type, const std::string& subtype,
      ComputeIntent intent) const;

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
