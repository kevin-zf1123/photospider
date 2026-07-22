// Photospider kernel: GraphRuntime per-graph state and execution routes
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

#include "compute/compute_request_coordinator.hpp"
#include "execution/execution_task_runtime.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_state_executor.hpp"
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
 * publishes one resource-neutral execution binding for each compute intent.
 * Every binding names a Host-lifetime physical route owned by the process
 * execution service. Compute events and execution traces are copied into
 * independent bounded public/internal pages without exposing worker, queue,
 * route-adapter, or cache ownership.
 *
 * @throws std::invalid_argument when an injected observation capacity or
 *         initial sequence is invalid.
 * @throws std::bad_alloc when graph, proxy, route-binding, or fixed-ring
 *         ownership cannot be allocated.
 * @throws std::filesystem::filesystem_error when runtime/cache directory
 *         preparation fails.
 * @note Visible Graph capture, mutation, predicate, and publication are
 *       serialized by `graph_state_`; same-Graph compute and route-binding
 *       inspection/replacement are serialized by `compute_requests_`. Event
 *       and trace rings have independent mutexes. The runtime owns its model,
 *       route values, cache-facing resources, and observation slots until
 *       destruction, but never owns physical execution resources.
 */
class GraphRuntime : public ExecutionHostContext {
 public:
  /**
   * @brief Copied private route binding for one compute intent.
   *
   * @throws Nothing for scalar operations; string copies may throw
   * `std::bad_alloc`.
   * @note The value contains no executor, worker, queue, reservation, native
   * device, or lifecycle authority.
   */
  struct ExecutionRouteBinding final {
    /** @brief Exact private route id. */
    std::string execution_type = "cpu";

    /** @brief Nonzero generation advanced by every successful replacement. */
    std::uint64_t generation = 1U;
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

    /** @brief Initial full high-precision route id. */
    std::string hp_execution_type = "cpu";

    /** @brief Initial real-time dirty-update route id. */
    std::string rt_execution_type = "cpu";

    /** @brief Fixed compute-event capacity for this runtime. */
    std::size_t compute_event_capacity = kComputeEventRingCapacity;

    /** @brief First compute-event sequence, or the exhausted sentinel. */
    uint64_t compute_event_initial_sequence = 1;

    /** @brief Initial compute-event shared drop count test seam. */
    uint64_t compute_event_initial_dropped_count = 0;

    /** @brief Fixed execution-trace capacity for this runtime. */
    std::size_t execution_trace_capacity = kExecutionTraceRingCapacity;

    /** @brief First execution-trace sequence, or the exhausted sentinel. */
    uint64_t execution_trace_initial_sequence = 1;

    /** @brief Initial unsequenced execution-trace drop count test seam. */
    uint64_t execution_trace_initial_dropped_count = 0;

    /** @brief First graph-wide supersession generation test seam. */
    uint64_t supersession_first_generation = 1;
  };

  /**
   * @brief Internal allocation-free execution-trace publication value.
   *
   * @throws Nothing for construction, copy, move, and scalar access.
   * @note `GraphRuntime` owns retained instances in its fixed ring and guards
   *       them with `log_mutex_`. Returned pages own independent copies whose
   *       lifetime and access no longer require the runtime lock.
   */
  struct ExecutionEvent {
    /** @brief Execution action recorded by a compute path. */
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

      /** @brief Captured task exception rethrown to the caller. */
      RETHROW_EXCEPTION,
    };

    /** @brief Per-runtime trace publication sequence. */
    uint64_t sequence;

    /** @brief Execution batch epoch. */
    uint64_t epoch;

    /** @brief Backend node id. */
    int node_id;

    /** @brief Worker id, or -1 when unavailable. */
    int worker_id;

    /** @brief Recorded execution action. */
    Action action;

    /** @brief Backend high-resolution observation time. */
    std::chrono::time_point<std::chrono::high_resolution_clock> timestamp;
  };

  /**
   * @brief Internal bounded non-destructive execution-trace page.
   *
   * @throws std::bad_alloc when event-vector construction, copy, or mutation
   *         allocates.
   * @note Metadata is computed with the event copy at one `log_mutex_` locked
   *       observation point. The returned value owns its event copies, needs
   *       no lock after return, and remains valid independently of later ring
   *       publication, eviction, clearing, or runtime destruction.
   */
  struct ExecutionEventPage {
    /** @brief Retained internal events whose sequence exceeds the cursor. */
    std::vector<ExecutionEvent> events;

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
   * @note Compute-event and execution-trace slots are fully allocated before
   *       publication begins. Construction consumes `info` before concurrent
   *       access begins; the runtime then retains exclusive ownership of its
   *       copied configuration and rings for its complete graph lifetime.
   */
  explicit GraphRuntime(const Info& info);
  /**
   * @brief Releases every graph-owned runtime resource.
   * @throws Nothing.
   * @note The compute-request lane first stops admission, drains accepted work,
   *       and joins its worker while graph-state remains available for final
   *       commit. Graph-state is then drained and joined before route values,
   *       GPU state, traces, and the model are released. Physical workers and
   *       route adapters remain owned by the Host-lifetime ExecutionService.
   *       An executor join invariant failure terminates rather than tearing
   *       down state beneath a live task.
   */
  ~GraphRuntime() noexcept override;

  /** @brief Prevents copying Graph, route, and observation ownership. */
  GraphRuntime(const GraphRuntime&) = delete;

  /** @brief Prevents copy assignment of graph-scoped runtime ownership. */
  GraphRuntime& operator=(const GraphRuntime&) = delete;

  /**
   * @brief Publishes this Graph runtime as running.
   * @return Nothing.
   * @throws Nothing.
   * @note Physical workers are owned by the Host-lifetime ExecutionService;
   * this call publishes only Graph-local lifecycle state after construction.
   */
  void start();

  /**
   * @brief Publishes this Graph runtime as stopped.
   * @return Nothing.
   * @throws Nothing.
   * @note The call is idempotent and does not stop Host-lifetime physical
   * workers. Kernel drains request and Graph-state lanes before calling it.
   */
  void stop();
  /**
   * @brief Reports whether this Graph runtime is running.
   * @return True after `start()` and before `stop()`.
   * @throws Nothing.
   * @note The acquire load pairs with lifecycle release stores.
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
   * @brief Admits one same-Graph compute or route-lifetime request.
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
   * @brief Prepares one canonical compute candidate and reserves key capacity.
   * @param key Target/request-intent supersession lineage.
   * @return Move-only identity and provisional ticket adoption.
   * @throws The allocation, admission, and synchronization errors documented
   * by compute::ComputeRequestCoordinator::prepare.
   * @note This is private Kernel infrastructure, not public Host ABI.
   */
  compute::ComputeRequestCoordinator::PreparedCandidate prepare_compute_request(
      const compute::SupersessionKey& key) {
    return compute_request_coordinator_.prepare(key);
  }

  /**
   * @brief Publishes one latest-wins compute candidate to the logical runner.
   * @param prepared Prepared identity and provisional ownership.
   * @param cancellation Request-wide cancellation fan-out source.
   * @param execute Existing synchronous Kernel compute path.
   * @param settle_superseded Exact-once completion for an unmaterialized loser.
   * @param settle_failure Fallback completion for an escaping callback.
   * @return Nothing after graph-state publication work is admitted.
   * @throws The validation, allocation, admission, and synchronization errors
   * documented by compute::ComputeRequestCoordinator::publish.
   * @note The admitted graph-state item linearizes later in FIFO order.
   * Execution uses only the existing compute-request lane worker; this method
   * creates no thread or asynchronous loop.
   */
  void publish_compute_request(
      compute::ComputeRequestCoordinator::PreparedCandidate prepared,
      std::shared_ptr<compute::ComputeRequestCancellationSource> cancellation,
      compute::ComputeRequestCoordinator::ExecuteCallback execute,
      compute::ComputeRequestCoordinator::SupersededCallback settle_superseded,
      compute::ComputeRequestCoordinator::FailureCallback settle_failure) {
    compute_request_coordinator_.publish(
        std::move(prepared), std::move(cancellation), std::move(execute),
        std::move(settle_superseded), std::move(settle_failure));
  }

  /**
   * @brief Tests exact request currency inside product commit arbitration.
   * @param identity Run-captured canonical key/generation.
   * @return True only for the latest graph-state-published generation.
   * @throws std::system_error for synchronization failure.
   */
  bool is_current_supersession(
      const compute::SupersessionIdentity& identity) const {
    return compute_request_coordinator_.is_current(identity);
  }

  /**
   * @brief Returns bounded coordinator/ticket ownership for private tests.
   * @return Scalar snapshot with no retained runtime ownership.
   * @throws std::system_error if coordinator or lane locking fails.
   * @note This observation is not installed Host ABI and may become stale
   * immediately after return.
   */
  compute::ComputeRequestCoordinator::Snapshot compute_request_snapshot()
      const {
    return compute_request_coordinator_.snapshot();
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
  void stop_compute_request_admission() {
    compute_request_coordinator_.stop_admission();
    compute_requests_.stop_admission();
  }

  /**
   * @brief Stops and drains every accepted private compute request.
   * @return Nothing after the request worker is joined.
   * @throws The lifecycle errors documented by
   *         GraphStateExecutor::close_and_drain.
   * @note Graph-state admission must remain available until this returns
   * because accepted requests may still submit their final predicate
   * transaction.
   */
  void close_compute_requests() {
    compute_request_coordinator_.stop_admission();
    compute_requests_.close_and_drain();
  }

  /**
   * @brief Restarts request admission after execution shutdown aborts close.
   * @return Nothing.
   * @throws The lifecycle errors documented by
   *         GraphStateExecutor::restart_after_close_failure.
   * @note Kernel restarts graph-state first, then this lane, so newly admitted
   *       compute can always capture visible state.
   */
  void restart_compute_requests_after_close_failure() {
    compute_requests_.restart_after_close_failure();
    compute_request_coordinator_.restart_after_close_failure();
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
   * @brief Publishes an execution trace with the current thread-local context.
   * @param action Execution action to record.
   * @param node_id Backend node id.
   * @return Nothing.
   * @throws Nothing.
   * @note Publication is sequenced and admitted to the fixed ring under the
   *       trace lock.
   */
  void log_event(ExecutionEvent::Action action, int node_id);

  /**
   * @brief Publishes an execution trace with explicit worker and epoch values.
   * @param action Execution action to record.
   * @param node_id Backend node id.
   * @param worker_id Worker id, or -1 when unavailable.
   * @param epoch Execution task epoch.
   * @return Nothing.
   * @throws Nothing.
   * @note Full-ring eviction and terminal exhaustion increment drop accounting
   *       with saturating arithmetic.
   */
  void log_event(ExecutionEvent::Action action, int node_id, int worker_id,
                 uint64_t epoch);

  /**
   * @brief Copies one bounded execution-trace page without removing entries.
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
  ExecutionEventPage execution_trace_page(uint64_t after_sequence,
                                          std::size_t limit) const;

  /**
   * @brief Removes all retained execution traces for deterministic tests.
   * @return Nothing.
   * @throws Nothing.
   * @note Sequence state is preserved, so later bounded reads report cleared
   *       history as a cursor gap. Production frontends have no clear method.
   */
  void clear_execution_trace();

  /**
   * @brief Reports a physical device capability to private execution routes.
   * @param device Stable public device label.
   * @return True for CPU and for an initialized Metal device on Apple hosts.
   * @throws Nothing.
   * @note No native device or command-queue handle crosses this boundary.
   */
  bool is_device_available(Device device) const noexcept override;

  /**
   * @brief Publishes execution worker identity into thread-local state.
   * @param worker_id Execution worker id, or -1 when unavailable.
   * @param epoch Active task epoch.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_task_context(int worker_id, uint64_t epoch) noexcept override;

  /**
   * @brief Clears execution worker identity from thread-local state.
   * @return Nothing.
   * @throws Nothing.
   */
  void clear_task_context() noexcept override;

  /**
   * @brief Maps and publishes one private execution trace action.
   * @param action Stable execution action.
   * @param node_id Backend node id, or -1 when unavailable.
   * @param worker_id Execution worker id, or -1 when unavailable.
   * @param epoch Active task epoch.
   * @return Nothing.
   * @throws Nothing; observation failures are dropped so task exceptions keep
   *         their original identity.
   * @note This is the only adapter-to-runtime execution-action mapping.
   */
  void log_event(ExecutionTraceAction action, int node_id, int worker_id,
                 uint64_t epoch) noexcept override;

  /**
   * @brief Returns execution worker identity for the calling thread.
   * @return Active compute or execution-context worker id, or -1.
   * @throws Nothing.
   */
  static int this_worker_id() noexcept;

  /**
   * @brief Returns execution task epoch for the calling thread.
   * @return Active execution epoch, or zero outside an execution callback.
   * @throws Nothing.
   */
  static uint64_t this_task_epoch() noexcept;

  /**
   * @brief Sets Host-observed execution identity on the calling thread.
   * @param worker_id Execution worker id, or -1.
   * @param epoch Execution task epoch.
   * @return Nothing.
   * @throws Nothing.
   * @note Private execution callbacks balance this helper through
   *       `ExecutionHostContext`; direct internal use must call
   *       `clear_execution_trace_context`.
   */
  static void set_execution_trace_context(int worker_id,
                                          uint64_t epoch) noexcept;

  /**
   * @brief Clears Host-observed execution identity on the calling thread.
   * @return Nothing.
   * @throws Nothing.
   */
  static void clear_execution_trace_context() noexcept;

  /**
   * @brief Returns the private native Metal device for internal compute code.
   * @return Borrowed Objective-C device, or null when unavailable.
   * @throws Nothing.
   * @note This native handle is never exposed through `ExecutionHostContext`.
   */
  id get_metal_device() noexcept;

  /**
   * @brief Returns the private native Metal command queue.
   * @return Borrowed Objective-C command queue, or null when unavailable.
   * @throws Nothing.
   * @note This native handle never crosses the private execution boundary.
   */
  id get_metal_command_queue() noexcept;

  /**
   * @brief Copies one intent's private route binding.
   * @param intent Global high precision or real-time update.
   * @return Host-owned route id and nonzero generation.
   * @throws std::invalid_argument for an unsupported intent.
   * @throws std::logic_error when no binding is published.
   * @throws std::bad_alloc while copying the route id.
   * @note Callers serialize active-request capture through the existing
   * compute-request lane when ordering against replacement is required.
   */
  ExecutionRouteBinding execution_route(ComputeIntent intent) const;

  /**
   * @brief Replaces one intent's route id as a nonallocating publication.
   * @param intent Global high precision or real-time update.
   * @param execution_type Exact `cpu`, `gpu_pipeline`, or `serial_debug` id.
   * @return Nothing after advancing the nonzero generation, including when
   * the type equals the previous value.
   * @throws std::invalid_argument for an invalid intent or route.
   * @throws GraphError when the generation is exhausted.
   * @throws std::bad_alloc while staging the new route id.
   * @note Validation and string allocation complete before the route mutex;
   * failure preserves the previous id/generation and creates no physical
   * owner, worker, queue, or reservation.
   */
  void replace_execution_route(ComputeIntent intent,
                               const std::string& execution_type);

  /**
   * @brief Tests whether one supported intent has a published route.
   * @param intent Candidate compute intent.
   * @return True only for an existing HP/RT route binding.
   * @throws Nothing.
   */
  bool has_execution_route(ComputeIntent intent) const noexcept;

 private:
  /** @brief Immutable copied construction and filesystem configuration. */
  Info info_;
  /** @brief Runtime-owned graph model and cache-facing state. */
  GraphModel model_;
  /** @brief Serial executor governing mutable graph model access. */
  GraphStateExecutor graph_state_;
  /**
   * @brief Bounded serial lane for compute/execution lifetime requests.
   * @note Its wrapper never exposes the bound model; member order drains this
   * lane before graph_state_ during ordinary reverse destruction.
   */
  GraphStateExecutor compute_requests_;
  /**
   * @brief Per-Graph latest-wins publication domain and logical ticket pump.
   * @note Reverse destruction releases this state before either executor lane.
   */
  compute::ComputeRequestCoordinator compute_request_coordinator_;
  /** @brief Fixed-capacity graph compute-event service. */
  GraphEventService event_service_;
  /** @brief Runtime-owned low-resolution real-time proxy graph. */
  std::unique_ptr<compute::RealtimeProxyGraph> realtime_proxy_graph_;

  /** @brief Intent routes to resource-neutral private execution ids. */
  std::map<ComputeIntent, ExecutionRouteBinding> execution_routes_;

  /** @brief Serializes copied route observation and publication. */
  mutable std::mutex execution_routes_mutex_;

  /** @brief True only after the runtime start transaction commits.
   */
  std::atomic<bool> running_{false};

  /** @brief Internal compute worker id for the calling thread. */
  static thread_local int tls_worker_id_;
  /** @brief Host-observed execution worker id for the calling thread. */
  static thread_local int tls_execution_context_worker_id_;
  /** @brief Host-observed execution epoch for the calling thread. */
  static thread_local uint64_t tls_execution_context_epoch_;

  /** @brief Private platform device/queue ownership hidden from policy plugins.
   */
  struct GpuContext;
  /** @brief Runtime-owned native GPU state, or null on unsupported hosts. */
  std::unique_ptr<GpuContext> gpu_context_;

  /** @brief Serializes execution-trace publication and page observation. */
  mutable std::mutex log_mutex_;

  /** @brief Fixed, constructor-allocated optional execution trace slots. */
  std::vector<std::optional<ExecutionEvent>> execution_trace_slots_;

  /** @brief Index of the oldest retained execution trace. */
  std::size_t execution_trace_head_ = 0;

  /** @brief Number of occupied execution trace slots. */
  std::size_t execution_trace_size_ = 0;

  /** @brief Next assignable trace sequence or exhausted sentinel. */
  uint64_t execution_trace_next_sequence_ = 1;

  /** @brief Saturating unsequenced exhausted-attempt test/accounting count. */
  uint64_t execution_trace_unsequenced_drops_ = 0;
};

}  // namespace ps
