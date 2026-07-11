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

#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_state_executor.hpp"
#include "kernel/scheduler/i_scheduler.hpp"  // M3.2: IScheduler 接口
#include "runtime/graph_event_service.hpp"

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
namespace compute {
class RealtimeProxyGraph;
}  // namespace compute

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
  /**
   * @brief Releases every scheduler and graph-owned runtime resource.
   * @throws Nothing.
   * @note Destructor cleanup attempts `stop()` but suppresses plugin lifecycle
   * exceptions; scheduler owners then retain their own no-throw fallback and
   * plugin-destroy ordering.
   */
  ~GraphRuntime() noexcept;

  GraphRuntime(const GraphRuntime&) = delete;
  GraphRuntime& operator=(const GraphRuntime&) = delete;

  /**
   * @brief Starts every attached scheduler as one runtime lifecycle
   * transaction.
   *
   * The runtime stages rollback tracking before invoking scheduler lifecycle
   * code. It publishes `running()==true` only after every previously stopped
   * scheduler starts successfully. On failure, schedulers started by this call
   * are shut down in reverse order and the original exception is rethrown.
   *
   * @return Nothing.
   * @throws std::bad_alloc if rollback tracking or a scheduler start exhausts
   * memory.
   * @throws std::system_error if a scheduler cannot create worker resources.
   * @throws Any exception propagated by a plugin scheduler's explicit start.
   * @note Rollback cleanup suppresses secondary shutdown failures to preserve
   * the original start exception. Scheduler objects and GraphModel remain owned
   * by this runtime; no graph cache or compute state is committed here.
   */
  void start();

  /**
   * @brief Stops all running schedulers owned by this graph runtime.
   * @return Nothing.
   * @throws The first exception propagated by a scheduler running-state query
   * or explicit shutdown.
   * @note The runtime publishes its stopped state under `schedulers_mutex_`,
   * then queries each scheduler and attempts shutdown whenever it reports
   * running or its state cannot be determined. A query failure therefore does
   * not skip that scheduler's cleanup, later schedulers are still swept, and
   * the first lifecycle error is rethrown only after the sweep. Graph/cache
   * ownership remains unchanged and repeated calls are lifecycle-idempotent
   * for built-ins.
   */
  void stop();
  /**
   * @brief Reports whether the complete scheduler set is running.
   * @return True only after the outer start transaction commits.
   * @throws Nothing.
   * @note The acquire load never exposes a partially started scheduler set.
   */
  bool running() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  std::vector<GraphEventService::ComputeEvent> drain_compute_events_now() {
    return event_service_.drain();
  }

  const Info& info() const { return info_; }
  GraphModel& model() { return model_; }
  GraphStateExecutor& graph_state() { return graph_state_; }
  GraphEventService& event_service() { return event_service_; }
  /**
   * @brief Returns the runtime-owned low-resolution RT proxy graph.
   *
   * @return Mutable proxy graph used by RealTimeUpdate dirty execution.
   * @throws Nothing.
   * @note The proxy graph is separate from GraphModel. Callers synchronize it
   * with the model under graph-state serialization before RT planning or
   * commit. It stores only transient RT output state keyed by node id.
   */
  compute::RealtimeProxyGraph& realtime_proxy_graph();

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

  /**
   * @brief Transactionally installs a scheduler for one compute intent.
   *
   * The method reserves the map slot, prepares the candidate with attach and,
   * when the runtime is running, start, then publishes ownership with a
   * non-allocating unique_ptr swap. An existing owner remains published and
   * alive until candidate preparation succeeds.
   *
   * @param intent Compute intent whose scheduler owner is installed.
   * @param scheduler Candidate owner; null removes an existing scheduler.
   * @return Nothing.
   * @throws std::bad_alloc If reserving a previously absent map slot fails.
   * @throws Any candidate attach/start exception unchanged after best-effort
   * shutdown and detach of that candidate.
   * @throws The first old-owner shutdown/detach exception after the candidate
   * has been published and both cleanup stages have been attempted.
   * @note Candidate failure leaves the prior map value and runtime running
   * state unchanged. This method shares the replacement transaction.
   */
  void set_scheduler(ComputeIntent intent,
                     std::unique_ptr<IScheduler> scheduler);

  /// @brief 获取指定意图的调度器
  /// @param intent 计算意图
  /// @return 调度器指针，如果不存在则返回 nullptr
  IScheduler* get_scheduler(ComputeIntent intent);
  const IScheduler* get_scheduler(ComputeIntent intent) const;

  /**
   * @brief Transactionally replaces the scheduler for one compute intent.
   *
   * Candidate attach/start completes before publication. If preparation fails,
   * candidate shutdown and detach are attempted independently and the exact
   * preparation exception is rethrown. On success, ownership is published by a
   * non-allocating swap; the displaced owner is then shut down, detached, and
   * destroyed in that order.
   *
   * @param intent Compute intent whose scheduler owner is replaced.
   * @param scheduler Candidate owner; null removes an existing scheduler.
   * @return Nothing.
   * @throws std::bad_alloc If reserving a previously absent map slot fails.
   * @throws Any candidate attach/start exception unchanged after rollback.
   * @throws The first displaced-owner shutdown/detach exception after
   * successful publication and completion of the cleanup sweep.
   * @note A displaced-owner cleanup error does not roll publication back. The
   * runtime running flag is never changed by this transaction.
   */
  void replace_scheduler(ComputeIntent intent,
                         std::unique_ptr<IScheduler> scheduler);

  /// @brief 检查是否有调度器注册到指定意图
  bool has_scheduler(ComputeIntent intent) const;

 private:
  Info info_;
  GraphModel model_;
  GraphStateExecutor graph_state_;
  GraphEventService event_service_;
  std::unique_ptr<compute::RealtimeProxyGraph> realtime_proxy_graph_;

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
