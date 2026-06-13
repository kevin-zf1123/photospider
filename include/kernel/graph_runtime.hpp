// Photospider kernel: GraphRuntime per-graph resources and scheduler registry
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "graph_model.hpp"  // NOLINT(build/include_subdir)
#include "kernel/graph_state_executor.hpp"
#include "kernel/scheduler/i_scheduler.hpp"  // M3.2: IScheduler 接口
#include "kernel/services/graph_event_service.hpp"

// [修改] 使用预处理器宏和前向声明来隔离平台特定的 Metal API
#ifdef __OBJC__
@protocol MTLDevice;
@protocol MTLCommandQueue;
#else
// 对于纯 C++ 文件，使用 void* 作为不透明指针
typedef void* id;
#endif

namespace ps {

class GraphRuntime;  // 前向声明

class GraphRuntime {
 public:
  struct Info {
    std::string name;
    std::filesystem::path root;
    std::filesystem::path yaml;
    std::filesystem::path config;
    std::filesystem::path cache_root;
  };

  struct SchedulerEvent {
    enum Action {
      ASSIGN_INITIAL,
      EXECUTE,
      EXECUTE_TILE,
      EXECUTE_DIRTY_SOURCE,
      EXECUTE_DIRTY_DOWNSTREAM_NODE,
      EXECUTE_DIRTY_DOWNSTREAM_TILE,
      SKIP_STALE_GENERATION,
      RETHROW_EXCEPTION,
    };
    uint64_t epoch;
    int node_id;
    int worker_id;
    Action action;
    std::chrono::time_point<std::chrono::high_resolution_clock> timestamp;
  };

  explicit GraphRuntime(const Info& info);
  ~GraphRuntime();

  GraphRuntime(const GraphRuntime&) = delete;
  GraphRuntime& operator=(const GraphRuntime&) = delete;

  void start();
  void stop();
  bool running() const { return running_; }

  std::vector<GraphEventService::ComputeEvent> drain_compute_events_now() {
    return event_service_.drain();
  }

  const Info& info() const { return info_; }
  GraphModel& model() { return model_; }
  GraphStateExecutor& graph_state() { return graph_state_; }
  GraphEventService& event_service() { return event_service_; }

  void log_event(SchedulerEvent::Action action, int node_id);
  void log_event(SchedulerEvent::Action action, int node_id, int worker_id,
                 uint64_t epoch);
  std::vector<SchedulerEvent> get_scheduler_log() const;
  void clear_scheduler_log();

  static int this_worker_id();
  static uint64_t this_task_epoch();
  static void set_scheduler_log_context(int worker_id, uint64_t epoch);
  static void clear_scheduler_log_context();

  id get_metal_device();
  id get_metal_command_queue();

  // =========================================================================
  // [M3.2 新增] 调度器管理 API
  // =========================================================================

  /// @brief 设置指定意图的调度器
  /// @param intent 计算意图（RT 或 HP）
  /// @param scheduler 调度器实例的唯一指针
  /// @note 如果已有调度器，会先 detach 旧调度器再设置新的
  /// @note 如果 runtime 已运行，新调度器会先 attach 到 runtime 再 start
  void set_scheduler(ComputeIntent intent,
                     std::unique_ptr<IScheduler> scheduler);

  /// @brief 获取指定意图的调度器
  /// @param intent 计算意图
  /// @return 调度器指针，如果不存在则返回 nullptr
  IScheduler* get_scheduler(ComputeIntent intent);
  const IScheduler* get_scheduler(ComputeIntent intent) const;

  /// @brief 替换指定意图的调度器（动态切换）
  /// @param intent 计算意图
  /// @param scheduler 新的调度器实例
  /// @note 此方法会先停止旧调度器，然后启动新调度器
  void replace_scheduler(ComputeIntent intent,
                         std::unique_ptr<IScheduler> scheduler);

  /// @brief 检查是否有调度器注册到指定意图
  bool has_scheduler(ComputeIntent intent) const;

 private:
  Info info_;
  GraphModel model_;
  GraphStateExecutor graph_state_;
  GraphEventService event_service_;

  // [M3.2 新增] 调度器映射表
  // 根据 ComputeIntent 路由到不同的调度器实例
  std::map<ComputeIntent, std::unique_ptr<IScheduler>> schedulers_;
  mutable std::mutex schedulers_mutex_;  // 保护 schedulers_ 的并发访问

  std::atomic<bool> running_{false};

  static thread_local int tls_worker_id_;
  static thread_local int tls_scheduler_log_worker_id_;
  static thread_local uint64_t tls_scheduler_log_epoch_;

  struct GpuContext;
  std::unique_ptr<GpuContext> gpu_context_;

  mutable std::mutex log_mutex_;
  std::vector<SchedulerEvent> scheduler_log_;
};

}  // namespace ps
