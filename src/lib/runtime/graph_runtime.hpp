// Photospider kernel: GraphRuntime per-graph resources and scheduler registry
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
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
  /**
   * @brief Internal construction inputs for one graph-owned runtime.
   *
   * @throws std::bad_alloc when string or filesystem-path value operations
   *         allocate.
   * @note Observation capacities, sequences, and drop counts are injectable
   *       deterministic-test seams. They do not expand public Host
   *       configuration ABI. This caller-owned value has no internal lock;
   *       configure it before construction. `GraphRuntime` copies the value
   *       and owns that copy for the complete runtime lifetime.
   */
  struct Info {
    /** @brief Loaded graph/session name. */
    std::string name;

    /** @brief Session filesystem root. */
    std::filesystem::path root;

    /** @brief Source graph YAML path. */
    std::filesystem::path yaml;

    /** @brief Session configuration path. */
    std::filesystem::path config;

    /** @brief Effective cache root, or empty to derive it from `root`. */
    std::filesystem::path cache_root;

    /** @brief Fixed compute-event capacity for this runtime. */
    std::size_t compute_event_capacity = kComputeEventRingCapacity;

    /** @brief First compute-event sequence, or the exhausted sentinel. */
    uint64_t compute_event_initial_sequence = 1;

    /** @brief Initial compute-event shared drop count test seam. */
    uint64_t compute_event_initial_dropped_count = 0;

    /** @brief Fixed scheduler-trace capacity for this runtime. */
    std::size_t scheduler_trace_capacity = kSchedulerTraceRingCapacity;

    /** @brief First scheduler-trace sequence, or the exhausted sentinel. */
    uint64_t scheduler_trace_initial_sequence = 1;

    /** @brief Initial unsequenced scheduler drop count test seam. */
    uint64_t scheduler_trace_initial_dropped_count = 0;
  };

  /**
   * @brief Internal allocation-free scheduler trace publication value.
   *
   * @throws Nothing for construction, copy, move, and scalar access.
   * @note `GraphRuntime` owns retained instances in its fixed ring and guards
   *       them with `log_mutex_`. Returned pages own independent copies whose
   *       lifetime and access no longer require the runtime lock.
   */
  struct SchedulerEvent {
    /** @brief Scheduler action recorded by a compute path. */
    enum Action {
      /** @brief Initial ready-task assignment. */
      ASSIGN_INITIAL,

      /** @brief Monolithic node execution. */
      EXECUTE,

      /** @brief Tiled node execution. */
      EXECUTE_TILE,

      /** @brief Dirty source execution. */
      EXECUTE_DIRTY_SOURCE,

      /** @brief Dirty downstream monolithic execution. */
      EXECUTE_DIRTY_DOWNSTREAM_NODE,

      /** @brief Dirty downstream tile execution. */
      EXECUTE_DIRTY_DOWNSTREAM_TILE,

      /** @brief Stale dirty generation skipped before execution. */
      SKIP_STALE_GENERATION,

      /** @brief Captured scheduler task exception rethrown to the caller. */
      RETHROW_EXCEPTION,
    };

    /** @brief Per-runtime trace publication sequence. */
    uint64_t sequence;

    /** @brief Scheduler task epoch. */
    uint64_t epoch;

    /** @brief Backend node id. */
    int node_id;

    /** @brief Worker id, or -1 when unavailable. */
    int worker_id;

    /** @brief Recorded scheduler action. */
    Action action;

    /** @brief Backend high-resolution observation time. */
    std::chrono::time_point<std::chrono::high_resolution_clock> timestamp;
  };

  /**
   * @brief Internal bounded non-destructive scheduler-trace page.
   *
   * @throws std::bad_alloc when event-vector construction, copy, or mutation
   *         allocates.
   * @note Metadata is computed with the event copy at one `log_mutex_` locked
   *       observation point. The returned value owns its event copies, needs
   *       no lock after return, and remains valid independently of later ring
   *       publication, eviction, clearing, or runtime destruction.
   */
  struct SchedulerEventPage {
    /** @brief Retained internal events whose sequence exceeds the cursor. */
    std::vector<SchedulerEvent> events;

    /** @brief Last event cursor, input cursor, or exhausted sentinel. */
    uint64_t next_sequence = 0;

    /** @brief Whether another matching retained event follows this page. */
    bool has_more = false;

    /** @brief Saturating exact history/exhaustion gap after the cursor. */
    uint64_t dropped_count = 0;
  };

  /**
   * @brief Creates all graph-owned model, observation, and platform resources.
   * @param info Filesystem inputs and internal observation-ring test seams.
   * @throws std::invalid_argument if an observation capacity or initial
   *         sequence is zero.
   * @throws std::bad_alloc if model, proxy graph, or preallocated ring storage
   *         cannot be created.
   * @throws std::filesystem::filesystem_error if session/cache directories
   *         cannot be created.
   * @note Compute-event and scheduler-trace slots are fully allocated before
   *       publication begins. Construction consumes `info` before concurrent
   *       access begins; the runtime then retains exclusive ownership of its
   *       copied configuration and rings for its complete graph lifetime.
   */
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

  /**
   * @brief Destructively drains one bounded compute-event batch.
   * @param limit Maximum events to remove.
   * @return Public sequenced event batch.
   * @throws std::invalid_argument for an invalid limit without mutation.
   * @throws std::bad_alloc if output allocation fails without mutation.
   * @note Delegates all locking and drop-reset semantics to the graph-owned
   *       event service.
   */
  ComputeEventBatch drain_compute_events_now(std::size_t limit) {
    return event_service_.drain(limit);
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

  /**
   * @brief Publishes a scheduler trace with the current thread-local context.
   * @param action Scheduler action to record.
   * @param node_id Backend node id.
   * @return Nothing.
   * @throws Nothing.
   * @note Publication is sequenced and admitted to the fixed ring under the
   *       trace lock.
   */
  void log_event(SchedulerEvent::Action action, int node_id);

  /**
   * @brief Publishes a scheduler trace with explicit worker and epoch values.
   * @param action Scheduler action to record.
   * @param node_id Backend node id.
   * @param worker_id Worker id, or -1 when unavailable.
   * @param epoch Scheduler task epoch.
   * @return Nothing.
   * @throws Nothing.
   * @note Full-ring eviction and terminal exhaustion increment drop accounting
   *       with saturating arithmetic.
   */
  void log_event(SchedulerEvent::Action action, int node_id, int worker_id,
                 uint64_t epoch);

  /**
   * @brief Copies one bounded scheduler-trace page without removing entries.
   * @param after_sequence Exclusive cursor; zero starts at the oldest retained
   *        entry and the exhausted sentinel requests a terminal empty page.
   * @param limit Maximum entries to copy.
   * @return Bounded internal page with cursor-specific drop metadata.
   * @throws std::invalid_argument for an invalid limit, future cursor, or an
   *         exhausted sentinel supplied before actual exhaustion.
   * @throws std::bad_alloc if bounded output allocation fails.
   * @note Copying, `has_more`, cursor advancement, and gap calculation observe
   *       one locked ring state.
   */
  SchedulerEventPage scheduler_trace_page(uint64_t after_sequence,
                                          std::size_t limit) const;

  /**
   * @brief Removes all retained scheduler traces for deterministic tests.
   * @return Nothing.
   * @throws Nothing.
   * @note Sequence state is preserved, so later bounded reads report cleared
   *       history as a cursor gap. Production frontends have no clear method.
   */
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
  /** @brief Fixed-capacity graph compute-event service. */
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

  /** @brief Serializes scheduler-trace publication and page observation. */
  mutable std::mutex log_mutex_;

  /** @brief Fixed, constructor-allocated optional scheduler trace slots. */
  std::vector<std::optional<SchedulerEvent>> scheduler_trace_slots_;

  /** @brief Index of the oldest retained scheduler trace. */
  std::size_t scheduler_trace_head_ = 0;

  /** @brief Number of occupied scheduler trace slots. */
  std::size_t scheduler_trace_size_ = 0;

  /** @brief Next assignable trace sequence or exhausted sentinel. */
  uint64_t scheduler_trace_next_sequence_ = 1;

  /** @brief Saturating unsequenced exhausted-attempt test/accounting count. */
  uint64_t scheduler_trace_unsequenced_drops_ = 0;
};

}  // namespace ps
