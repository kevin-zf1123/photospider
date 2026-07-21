// Photospider kernel: GraphRuntime per-graph resources and scheduler registry
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_state_executor.hpp"
#include "photospider/scheduler/scheduler.hpp"
#include "runtime/graph_event_service.hpp"

#ifdef __OBJC__
@protocol MTLDevice;
@protocol MTLCommandQueue;
#else
/** @brief Opaque Objective-C object placeholder for pure C++ translation units.
 */
typedef void* id;
#endif

namespace ps {

class GraphRuntime;
namespace compute {
class RealtimeProxyGraph;
}  // namespace compute

/**
 * @brief Owns one graph model, graph-state and compute-request lanes, execution
 *        bindings, and bounded observation rings.
 *
 * The runtime constructs all graph-scoped resources from `Info`, routes
 * lifecycle-sensitive graph operations through `GraphStateExecutor`, and
 * publishes one execution binding for each compute intent. Legacy bindings
 * own scheduler instances; built-in CPU bindings refer to the ownerless
 * process service. Compute events and scheduler traces are copied into
 * independent bounded public/internal pages without exposing scheduler or
 * cache ownership.
 *
 * @throws std::invalid_argument when an injected observation capacity or
 *         initial sequence is invalid.
 * @throws std::bad_alloc when graph, legacy scheduler, proxy, or fixed-ring
 *         ownership cannot be allocated.
 * @throws std::filesystem::filesystem_error when runtime/cache directory
 *         preparation fails.
 * @note Visible Graph capture, mutation, predicate, and publication are
 *       serialized by `graph_state_`; same-Graph compute and scheduler-owner
 *       inspection/replacement are serialized by `compute_requests_`. Event
 *       and trace rings have independent mutexes. The runtime owns every model,
 *       legacy scheduler, cache-facing resource, and observation slot until
 *       destruction; a process-service route is an ownerless binding.
 */
class GraphRuntime : public SchedulerHostContext {
 public:
  /**
   * @brief Private physical-dispatch route paired with one intent binding.
   *
   * The value distinguishes transitional per-Graph scheduler execution from
   * issue #69's built-in CPU HP/RT service route. It carries no policy,
   * admission, resource reservation, or public scheduler ABI state.
   *
   * @throws Nothing for construction, copy, and scalar access.
   * @note Scheduler planning and replacement publish this value in the same
   * registry transaction as the optional legacy scheduler owner.
   */
  struct SchedulerExecutionRoute {
    /**
     * @brief Physical owner selected for current ready task execution.
     *
     * @throws Nothing for value operations.
     */
    enum class Domain {
      /** @brief Existing scheduler instance executes its own ready work. */
      PerGraphScheduler,

      /** @brief Injected process CPU ExecutionService executes HP/RT work. */
      ProcessCpuService,
    };

    /** @brief Selected physical execution owner. */
    Domain domain = Domain::PerGraphScheduler;
  };

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
   * @note The compute-request lane first stops admission, drains accepted work,
   *       and joins its worker while graph-state remains available for final
   *       commit. Graph-state is then drained and joined. Scheduler shutdown
   * and detach run in order under the registry lock before later
   *       GPU/trace/model teardown. Scheduler lifecycle exceptions are
   *       suppressed; an executor join invariant failure terminates rather than
   *       tearing down state beneath a live task.
   */
  ~GraphRuntime() noexcept override;

  /** @brief Prevents copying graph, scheduler, and observation ownership. */
  GraphRuntime(const GraphRuntime&) = delete;

  /** @brief Prevents copy assignment of graph-scoped runtime ownership. */
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

  /**
   * @brief Returns immutable graph construction/configuration inputs.
   * @return Borrowed runtime-owned configuration reference.
   * @throws Nothing.
   * @note The reference remains valid for this runtime lifetime.
   */
  const Info& info() const noexcept { return info_; }

  /**
   * @brief Returns the runtime-owned mutable graph model.
   * @return Borrowed graph model reference.
   * @throws Nothing.
   * @note Mutating callers must enter `graph_state()` serialization.
   */
  GraphModel& model() noexcept { return model_; }

  /**
   * @brief Returns the serialized graph-state execution lane.
   * @return Borrowed runtime-owned executor reference.
   * @throws Nothing.
   */
  GraphStateExecutor& graph_state() noexcept { return graph_state_; }

  /**
   * @brief Admits one same-Graph compute or scheduler-lifetime request.
   *
   * @tparam Fn No-argument callable retained by the private bounded serial
   * request lane.
   * @param fn Request callback captured by value.
   * @return Future carrying the callable's exact result or exception.
   * @throws std::bad_alloc if task/future capture allocation fails.
   * @throws std::logic_error on request-lane worker reentry.
   * @throws std::runtime_error after compute-request admission stops.
   * @throws std::system_error for queue synchronization failures.
   * @note The wrapper deliberately hides the GraphModel reference required by
   *       its reused executor mechanism. The callback must enter graph_state()
   *       separately for capture, mutation, predicate, or publication. Future
   *       destruction neither cancels nor waits for accepted work.
   */
  template <typename Fn>
  auto submit_compute_request(Fn&& fn)
      -> std::future<std::invoke_result_t<Fn>> {
    using Ret = std::invoke_result_t<Fn>;
    return compute_requests_.submit(
        [f = std::forward<Fn>(fn)](GraphModel&) mutable -> Ret {
          if constexpr (std::is_void_v<Ret>) {
            std::invoke(f);
          } else {
            return std::invoke(f);
          }
        });
  }

  /**
   * @brief Stops new private compute-request admission without draining work.
   * @return Nothing.
   * @throws The lifecycle errors documented by
   * GraphStateExecutor::stop_admission.
   * @note Host close calls this before waiting accepted async work so a
   * producer blocked on the bounded request queue is rejected and can retire
   * its pre-registration.
   */
  void stop_compute_request_admission() { compute_requests_.stop_admission(); }

  /**
   * @brief Stops and drains every accepted private compute request.
   * @return Nothing after the request worker is joined.
   * @throws The lifecycle errors documented by
   *         GraphStateExecutor::close_and_drain.
   * @note Graph-state admission must remain available until this returns
   * because accepted requests may still submit their final predicate
   * transaction.
   */
  void close_compute_requests() { compute_requests_.close_and_drain(); }

  /**
   * @brief Restarts request admission after scheduler shutdown aborts close.
   * @return Nothing.
   * @throws The lifecycle errors documented by
   *         GraphStateExecutor::restart_after_close_failure.
   * @note Kernel restarts graph-state first, then this lane, so newly admitted
   *       compute can always capture visible state.
   */
  void restart_compute_requests_after_close_failure() {
    compute_requests_.restart_after_close_failure();
  }

  /**
   * @brief Returns the graph-owned compute-event service.
   * @return Borrowed event-service reference.
   * @throws Nothing.
   * @note The service owns its own synchronization and bounded ring.
   */
  GraphEventService& event_service() noexcept { return event_service_; }
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

  /**
   * @brief Reports a physical device capability to attached schedulers.
   * @param device Stable public device label.
   * @return True for CPU and for an initialized Metal device on Apple hosts.
   * @throws Nothing.
   * @note No native device or command-queue handle crosses this boundary.
   */
  bool is_device_available(Device device) const noexcept override;

  /**
   * @brief Publishes scheduler worker identity into host thread-local state.
   * @param worker_id Scheduler worker id, or -1 when unavailable.
   * @param epoch Active task epoch.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_task_context(int worker_id, uint64_t epoch) noexcept override;

  /**
   * @brief Clears scheduler worker identity from host thread-local state.
   * @return Nothing.
   * @throws Nothing.
   */
  void clear_task_context() noexcept override;

  /**
   * @brief Maps and publishes one public scheduler trace action.
   * @param action Stable public scheduler action.
   * @param node_id Backend node id, or -1 when unavailable.
   * @param worker_id Scheduler worker id, or -1 when unavailable.
   * @param epoch Active task epoch.
   * @return Nothing.
   * @throws Nothing; observation failures are dropped so task exceptions keep
   *         their original identity.
   * @note This is the only public-to-private scheduler action mapping.
   */
  void log_event(SchedulerTraceAction action, int node_id, int worker_id,
                 uint64_t epoch) noexcept override;

  /**
   * @brief Returns scheduler worker identity for the calling thread.
   * @return Active compute worker id, scheduler-context id, or -1.
   * @throws Nothing.
   */
  static int this_worker_id() noexcept;

  /**
   * @brief Returns scheduler task epoch for the calling thread.
   * @return Active scheduler epoch, or zero outside a scheduler callback.
   * @throws Nothing.
   */
  static uint64_t this_task_epoch() noexcept;

  /**
   * @brief Sets host-observed scheduler identity on the calling thread.
   * @param worker_id Scheduler worker id, or -1.
   * @param epoch Scheduler task epoch.
   * @return Nothing.
   * @throws Nothing.
   * @note Scheduler callbacks balance this helper through the public host
   *       context; direct internal use must call `clear_scheduler_log_context`.
   */
  static void set_scheduler_log_context(int worker_id, uint64_t epoch) noexcept;

  /**
   * @brief Clears host-observed scheduler identity on the calling thread.
   * @return Nothing.
   * @throws Nothing.
   */
  static void clear_scheduler_log_context() noexcept;

  /**
   * @brief Returns the private native Metal device for internal compute code.
   * @return Borrowed Objective-C device, or null when unavailable.
   * @throws Nothing.
   * @note This native handle is never exposed through `SchedulerHostContext`.
   */
  id get_metal_device() noexcept;

  /**
   * @brief Returns the private native Metal command queue.
   * @return Borrowed Objective-C command queue, or null when unavailable.
   * @throws Nothing.
   * @note This native handle is never exposed through the scheduler SDK.
   */
  id get_metal_command_queue() noexcept;

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
   * @note Candidate failure leaves the prior map value and runtime running
   * state unchanged. This method shares the replacement transaction. After
   * successful publication, displaced-owner shutdown/detach failures are
   * suppressed as post-commit diagnostics; the old owner is still destroyed
   * and the committed replacement returns normally.
   */
  void set_scheduler(ComputeIntent intent,
                     std::unique_ptr<IScheduler> scheduler);

  /**
   * @brief Transactionally installs a scheduler and explicit execution route.
   * @param intent Compute intent whose scheduler owner is installed.
   * @param scheduler Candidate owner; null publishes an ownerless
   * process-service route or removes a legacy scheduler.
   * @param execution_route Private physical route published with the owner.
   * @return Nothing.
   * @throws std::invalid_argument when a process-service route carries a
   * Graph-owned scheduler.
   * @throws Any replacement transaction exception documented above.
   * @note This overload is used only by trusted Kernel scheduler planning.
   */
  void set_scheduler(ComputeIntent intent,
                     std::unique_ptr<IScheduler> scheduler,
                     SchedulerExecutionRoute execution_route);

  /**
   * @brief Looks up the scheduler for one compute intent.
   * @param intent Compute intent route.
   * @return Borrowed scheduler pointer, or null when no owner is installed.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note The pointer is stable only while scheduler replacement/destruction is
   *       excluded, normally by the graph-state execution lane.
   */
  IScheduler* get_scheduler(ComputeIntent intent);

  /**
   * @brief Looks up a read-only scheduler for one compute intent.
   * @param intent Compute intent route.
   * @return Borrowed const scheduler pointer, or null when absent.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note The pointer has the same external serialization requirement as the
   *       mutable overload.
   */
  const IScheduler* get_scheduler(ComputeIntent intent) const;

  /**
   * @brief Returns the physical execution route paired with one intent.
   * @param intent Compute intent whose private route is inspected.
   * @return Published route, or the legacy per-Graph default when absent.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note The value is copied under the same registry lock as scheduler
   * ownership. It grants no scheduler lifetime or resource authority.
   */
  SchedulerExecutionRoute get_scheduler_execution_route(
      ComputeIntent intent) const;

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
   * @param scheduler Candidate owner; null publishes an ownerless
   * process-service route or removes a legacy scheduler.
   * @return Nothing.
   * @throws std::bad_alloc If reserving a previously absent map slot fails.
   * @throws Any candidate attach/start exception unchanged after rollback.
   * @note A displaced-owner cleanup error is suppressed after both lifecycle
   * stages are attempted, because publication has already committed and cannot
   * truthfully be reported as failed. The displaced owner is still destroyed,
   * and the runtime running flag is never changed by this transaction.
   */
  void replace_scheduler(ComputeIntent intent,
                         std::unique_ptr<IScheduler> scheduler);

  /**
   * @brief Replaces a scheduler and explicit physical route atomically.
   * @param intent Compute intent whose scheduler owner is replaced.
   * @param scheduler Candidate owner; null removes an existing scheduler.
   * @param execution_route Private physical route published with the owner.
   * @return Nothing.
   * @throws std::invalid_argument when a process-service route carries a
   * Graph-owned scheduler.
   * @throws Any replacement transaction exception documented above.
   * @note Trusted Kernel planning uses this overload; direct callers retain
   * the legacy per-Graph route through the two-argument overload.
   */
  void replace_scheduler(ComputeIntent intent,
                         std::unique_ptr<IScheduler> scheduler,
                         SchedulerExecutionRoute execution_route);

  /**
   * @brief Tests whether one compute intent has a usable execution binding.
   * @param intent Compute intent route.
   * @return True for a legacy scheduler owner or process CPU service route.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note This query reports execution availability, not Graph-local
   * scheduler ownership.
   */
  bool has_scheduler(ComputeIntent intent) const;

 private:
  /**
   * @brief Transactionally published scheduler owner and execution route.
   *
   * @throws Nothing for default construction and non-allocating swaps.
   * @note The route and optional legacy scheduler owner share one map slot and
   * publication transaction. Process-service bindings intentionally have no
   * Graph-owned scheduler.
   */
  struct SchedulerBinding {
    /**
     * @brief Exclusively owned legacy scheduler, null for process CPU routes.
     */
    std::unique_ptr<IScheduler> scheduler;

    /** @brief Private physical route published with that owner. */
    SchedulerExecutionRoute execution_route;
  };

  /** @brief Immutable copied construction and filesystem configuration. */
  Info info_;
  /** @brief Runtime-owned graph model and cache-facing state. */
  GraphModel model_;
  /** @brief Serial executor governing mutable graph model access. */
  GraphStateExecutor graph_state_;
  /**
   * @brief Bounded serial lane for compute/scheduler lifetime requests.
   * @note Its wrapper never exposes the bound model; member order drains this
   * lane before graph_state_ during ordinary reverse destruction.
   */
  GraphStateExecutor compute_requests_;
  /** @brief Fixed-capacity graph compute-event service. */
  GraphEventService event_service_;
  /** @brief Runtime-owned low-resolution real-time proxy graph. */
  std::unique_ptr<compute::RealtimeProxyGraph> realtime_proxy_graph_;

  /** @brief Intent routes to atomically paired scheduler owners and routes. */
  std::map<ComputeIntent, SchedulerBinding> schedulers_;
  /** @brief Serializes scheduler registry and lifecycle transactions. */
  mutable std::mutex schedulers_mutex_;

  /** @brief True only after the complete scheduler start transaction commits.
   */
  std::atomic<bool> running_{false};

  /** @brief Internal compute worker id for the calling thread. */
  static thread_local int tls_worker_id_;
  /** @brief Host-observed scheduler worker id for the calling thread. */
  static thread_local int tls_scheduler_log_worker_id_;
  /** @brief Host-observed scheduler epoch for the calling thread. */
  static thread_local uint64_t tls_scheduler_log_epoch_;

  /** @brief Private platform device/queue ownership hidden from scheduler SDK.
   */
  struct GpuContext;
  /** @brief Runtime-owned native GPU state, or null on unsupported hosts. */
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
