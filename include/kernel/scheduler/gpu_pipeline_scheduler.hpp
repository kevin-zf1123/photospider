// Photospider kernel: GpuPipelineScheduler
// M3.5: 异构调度器 - 支持 HP 走 GPU、RT 走 CPU 的混合计算模式
// M3.6: Node-Level 调度 - Scheduler 内部优先级表决策
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
// PriorityEntry: 优先级表条目
// 用于 Scheduler 内部决定实现选择策略
// =============================================================================
struct PriorityEntry {
  // 目标设备
  Device device{Device::CPU};

  // 是否偏好 Monolithic（整图计算）
  bool prefer_monolithic{false};

  // Tile 大小偏好
  TileSizePreference tile_pref{TileSizePreference::UNDEFINED};

  // 优先级（越低越优先）
  int priority{100};

  // 描述（用于调试）
  std::string description;
};

// =============================================================================
// GpuPipelineScheduler: GPU Pipeline 调度器
// 实现异构调度：HP 优先使用 GPU，RT 优先使用 CPU 以保证低延迟
// 支持 RT 和 HP 同时调度，其中 RT 优先级高于 HP
// M3.6: 实现 Node-Level 调度，内部持有优先级表做实现选择
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
    // Micro tile 大小，具体含义由 compute domain 决定
    int micro_tile_size;
    // Macro tile 大小，具体含义由 compute domain 决定
    int macro_tile_size;
    // 聚合阈值（超过此数量的 micro tile 会被聚合）
    int aggregation_threshold;

    Config()
        : gpu_workers(1),
          cpu_workers(0),
          prefer_gpu_for_hp(true),
          force_cpu_for_rt(true),
          rt_preempt_threshold_ms(16),
          micro_tile_size(16),
          macro_tile_size(256),
          aggregation_threshold(4) {}
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
  // Node-Level 调度接口 (M3.6 新增)
  // ---------------------------------------------------------------------------

  /// @brief Node-Level 调度入口
  std::future<NodeOutput> schedule_node(const NodeScheduleRequest& request,
                                        GraphModel& graph) override;

  /// @brief 检查是否应该聚合为 Macro task
  bool should_aggregate_to_macro(const TaskGroup& group) const override;

  /// @brief 是否支持 Node-Level 调度
  bool supports_node_level_scheduling() const override { return true; }

  /// @brief 是否支持 TaskGroup 聚合
  bool supports_task_group_aggregation() const override { return true; }

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
    ScheduledTask(uint64_t e, Task&& t,
                  ComputeIntent i = ComputeIntent::GlobalHighPrecision)
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
  const OpImplementation* select_implementation(const std::string& type,
                                                const std::string& subtype,
                                                ComputeIntent intent) const;

  // [M3.6] 使用优先级表选择最优实现
  const OpImplementation* select_impl_with_priority(const std::string& type,
                                                    const std::string& subtype,
                                                    ComputeIntent intent) const;

  // [M3.6] 初始化优先级表
  void init_priority_tables();

  // [M3.6] 将 ROI 切分为 tile 列表
  std::vector<cv::Rect> split_roi_to_tiles(const cv::Rect& roi,
                                           int tile_size) const;

  // [M3.6] 执行 Node-Level 计算
  NodeOutput execute_node_compute(const NodeScheduleRequest& request,
                                  GraphModel& graph);

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
  std::atomic<uint64_t> node_level_tasks_scheduled_{0};  // M3.6
  std::atomic<uint64_t> task_groups_aggregated_{0};      // M3.6

  // Thread-local storage
  static thread_local int tls_worker_id_;
  static thread_local uint64_t tls_active_epoch_;
  static thread_local bool tls_is_gpu_worker_;

  // ---------------------------------------------------------------------------
  // [M3.6] 优先级表
  // Scheduler 内部持有，用于决定实现选择策略
  // ---------------------------------------------------------------------------

  // HP 模式优先级表；Macro/Micro 是 HP domain 内的粒度偏好
  std::vector<PriorityEntry> hp_priority_table_;

  // RT 模式优先级表；Macro/Micro 是 RT domain 内的粒度偏好
  std::vector<PriorityEntry> rt_priority_table_;

  // TaskGroup ID 计数器
  std::atomic<uint64_t> task_group_id_counter_{0};
};

}  // namespace ps
