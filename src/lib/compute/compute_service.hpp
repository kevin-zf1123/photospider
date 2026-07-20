#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "compute/compute_run.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {

class GraphRuntime;
class GraphTraversalService;
class GraphCacheService;
class GraphEventService;
struct BenchmarkEvent;
namespace compute {
class DirtyNodeSynchronization;
class DirtySiblingCommitGate;
class ExecutionService;
class RealtimeProxyGraph;
class StabilizedDirtyParameters;
}  // namespace compute

/**
 * @brief Internal compute facade coordinating request-scoped collaborators.
 *
 * ComputeService exposes C++ members to Kernel and backend tests inside the
 * private source tree. Those members are not the installable product public
 * API and are not a frontend or plugin ABI; external callers use `ps::Host`.
 *
 * @note The facade borrows graph services supplied at construction and never
 * owns GraphRuntime or GraphModel. Callers serialize visible graph mutation at
 * the Kernel graph-state boundary.
 * @throws std::bad_alloc from compute members when request planning, operation
 * execution, cache, telemetry, or dirty-state storage exhausts memory.
 */
class ComputeService {
 public:
  /**
   * @brief Cache controls for one ComputeService request.
   *
   * CacheOptions names the cache precision and cache invalidation switches that
   * used to be passed as adjacent positional parameters. The struct is copied
   * only for the duration of a request and never stores graph-owned state.
   *
   * @note disable_disk_cache affects disk reads only. In-memory cache clearing
   * is controlled by force_recache and remains scoped to the planned target
   * cone.
   */
  struct CacheOptions {
    /** @brief Precision label used when reading or writing disk cache data. */
    std::string precision;

    /**
     * @brief Clears selected in-memory caches before executing the request.
     *
     * @note Also invalidates cached full task graph expansions before planning
     * so input-shape changes rebuild tiled task ROIs. For HP dirty updates this
     * forces a full-frame HP plan because the staging buffer intentionally does
     * not seed old HP pixels.
     */
    bool force_recache = false;

    /** @brief Prevents disk-cache reads for the active request. */
    bool disable_disk_cache = false;
  };

  /**
   * @brief Timing and benchmark sinks for one ComputeService request.
   *
   * TelemetryOptions separates observability settings from cache and execution
   * policy so callers do not need to reason about multiple unrelated boolean
   * positions. benchmark_events remains caller-owned and is never retained
   * beyond the active compute call.
   *
   * @note enable_timing controls graph TimingCollector mutation and
   * GraphEventService timing payloads. benchmark_events may be null when the
   * caller only needs graph-level timing or no timing at all.
   */
  struct TelemetryOptions {
    /** @brief Whether graph timing results should be reset and collected. */
    bool enable_timing = false;

    /** @brief Optional caller-owned sink for per-node benchmark events. */
    std::vector<BenchmarkEvent>* benchmark_events = nullptr;
  };

  /**
   * @brief Immutable compute request accepted by the ComputeService facade.
   *
   * Request is the C++ member contract used inside the private compute service
   * for sequential, parallel, high-precision, and dirty/real-time compute. It
   * is not an installable product public API. A missing intent preserves the
   * legacy HP-only path, while a populated intent delegates to
   * IntentUpdateCoordinator with dirty_roi forwarded unchanged.
   *
   * @note The request does not own GraphModel, GraphRuntime, schedulers, or
   * cache entries. Every created Run snapshots graph_identity and qos before
   * planning: one Run for a standalone HP request and one immutable copy in
   * each HP/RT child of a realtime request. All graph mutation happens inside
   * the compute call selected by compute() or compute_parallel().
   */
  struct Request {
    /** @brief Target graph node id requested by the caller. */
    int node_id = -1;

    /** @brief Cache precision, force-recache, and disk-read controls. */
    CacheOptions cache;

    /** @brief Timing and benchmark recording controls. */
    TelemetryOptions telemetry;

    /** @brief Optional compute intent; nullopt selects legacy HP behavior. */
    std::optional<ComputeIntent> intent;

    /** @brief Optional HP-space dirty ROI for dirty HP or RT updates. */
    std::optional<PixelRect> dirty_roi;

    /**
     * @brief Stable graph/session identity captured in every created Run.
     *
     * @note Kernel supplies its session label. Direct private service callers
     * may leave this empty without manufacturing address-based identity.
     */
    std::string graph_identity;

    /**
     * @brief Explicit QoS value captured independently from compute intent.
     *
     * @note Current Kernel callers use Throughput, weight one, no deadline, and
     * an optional sequential parallelism cap. Current Runs record but do not
     * enforce scheduling policy from this value.
     */
    compute::ComputeRunQos qos;
  };

  /**
   * @brief Runtime policy for one ComputeService execution path.
   *
   * ExecutionStrategy captures whether the caller selected an execution-bound
   * path and, when so, which runtime supplies Graph lifecycle, binding lookup,
   * and worker-facing observation. It keeps execution policy separate from
   * the semantic request so the same Request can be used by both compute() and
   * compute_parallel().
   *
   * @note A null runtime is valid only when use_parallel_executor is false.
   * Parallel HP or RT scheduling validates the runtime before dispatching work.
   */
  struct ExecutionStrategy {
    /**
     * @brief Runtime providing bindings and observation, or null for inline
     * work.
     *
     * @note Legacy routes borrow Graph-owned scheduler pools through this
     * runtime. An ownerless built-in CPU binding instead dispatches through the
     * injected process ExecutionService.
     */
    GraphRuntime* runtime = nullptr;

    /** @brief Whether full non-dirty work should use scheduler dispatch. */
    bool use_parallel_executor = false;
  };

  /**
   * @brief Constructs a compute facade that borrows graph support services.
   *
   * @param traversal Traversal service used for dependency order and dirty
   * planning.
   * @param cache Cache service used for disk cache reads and writes.
   * @param events Event service used for compute status and timing events.
   * @param execution_service Explicit process CPU execution owner.
   * @throws Nothing directly; referenced services must already be valid.
   * @note ComputeService does not own the supplied services and must not
   * outlive them. Built-in CPU full, dirty, and RT paths share the injected
   * execution service; inline work and legacy plugin/GPU/serial routes may
   * execute without it.
   */
  ComputeService(GraphTraversalService& traversal, GraphCacheService& cache,
                 GraphEventService& events,
                 compute::ExecutionService& execution_service);

  /**
   * @brief Destroys inline RT proxy graph storage owned by the service.
   *
   * @throws Nothing.
   * @note Runtime-backed compute uses GraphRuntime-owned proxy graphs. The
   * inline store exists only for direct ComputeService::compute() callers that
   * do not supply a GraphRuntime.
   */
  ~ComputeService();

  /**
   * @brief Executes a non-parallel compute request against one graph model.
   *
   * @param graph Graph whose node caches, timing, and inspection state are
   * read and mutated.
   * @param request Target, cache, telemetry, intent, and dirty ROI options.
   * @return Mutable output selected by the request. HP outputs are owned by
   * graph node state; RT dirty outputs are owned by the service inline proxy
   * graph.
   * @throws GraphError for validation, planning, cache, dispatch, or missing
   * output failures; may propagate operation-specific exceptions.
   * @throws std::bad_alloc unchanged when any Host-reachable compute stage
   * exhausts memory.
   * @note A request without intent uses the legacy GlobalHighPrecision
   * recursive path. Intent-aware requests are coordinated inline without a
   * scheduler runtime.
   */
  NodeOutput& compute(GraphModel& graph, const Request& request);

  /**
   * @brief Executes an execution-bound compute request against one graph.
   *
   * @param graph Graph whose node caches, timing, and inspection state are
   * read and mutated.
   * @param runtime Runtime providing Graph lifecycle, intent bindings, and
   * worker-facing observation. Legacy bindings expose Graph-owned schedulers;
   * ownerless built-in CPU bindings use the injected ExecutionService.
   * @param request Target, cache, telemetry, intent, and dirty ROI options.
   * @return Mutable output selected by the request. HP outputs are owned by
   * graph node state; RT dirty outputs are owned by the runtime proxy graph.
   * @throws GraphError for scheduler lookup, validation, planning, dispatch, or
   * missing output failures; may propagate operation-specific exceptions.
   * @throws std::bad_alloc unchanged when planning, task dispatch, operation,
   * cache, telemetry, or result storage exhausts memory.
   * @note Dirty RT updates create separate HP and RT child Runs. Built-in CPU
   * routes submit both children to the fixed process ExecutionService, while
   * plugin, GPU, and serial routes retain their Graph-owned schedulers. The
   * staged dirty commit path starts RT before HP, waits for RT proxy commit
   * before HP graph commit, and returns the RT proxy output. Kernel callers
   * must enter this method from GraphStateExecutor so execution-bound work
   * remains serialized with graph-state operations.
   */
  NodeOutput& compute_parallel(GraphModel& graph, GraphRuntime& runtime,
                               const Request& request);

 private:
  /**
   * @brief Shared recursive sequential compute context.
   *
   * RecursiveComputeContext carries request-wide cache, timing, cycle-detection
   * and benchmark state while compute_internal changes only the current node
   * id. The referenced objects are owned by compute_sequential_impl and remain
   * valid for the full recursive call tree.
   *
   * @note allow_disk_cache is calculated once from force_recache and
   * disable_disk_cache so recursive calls cannot accidentally diverge.
   */
  struct RecursiveComputeContext {
    /** @brief Precision label used when saving disk cache entries. */
    const std::string& cache_precision;

    /** @brief Per-request recursion stack used to detect graph cycles. */
    std::unordered_map<int, bool>& visiting;

    /** @brief Whether graph timing results should be collected. */
    bool enable_timing = false;

    /** @brief Whether recursive nodes may read disk cache entries. */
    bool allow_disk_cache = false;

    /** @brief Optional caller-owned benchmark sink for this request. */
    std::vector<BenchmarkEvent>* benchmark_events = nullptr;
  };

  /**
   * @brief Recursively computes one HP node for the sequential service path.
   *
   * @param graph Graph whose topology, cache, timing, and event state are read
   * and updated.
   * @param node_id Node to compute after its dependencies.
   * @param context Request-owned recursion, cache, timing, and benchmark state.
   * @return Mutable HP output owned by the target graph node.
   * @throws std::bad_alloc unchanged when dependency, operation, cache,
   * telemetry, or result storage exhausts memory.
   * @throws GraphError for cycles, missing dependencies or operations, cache
   * failures, and operation failures represented by the backend.
   * @note The context and its references must outlive the recursive call tree.
   * Effective parameters and execution-facing node state remain on a
   * request-local Node snapshot. The method commits only the resolved
   * graph-owned input-size hint, HP cache/version, disk-cache effects, and
   * telemetry on the calling thread.
   */
  NodeOutput& compute_internal(GraphModel& graph, int node_id,
                               const RecursiveComputeContext& context);

  /**
   * @brief Delegates one HP dirty update to HighPrecisionDirtyExecutor.
   *
   * @param graph Graph whose HP state and cache are updated.
   * @param proxy_graph RT proxy graph receiving optional HP downsample output.
   * @param strategy Inline or execution-bound execution policy.
   * @param request Validated dirty request and telemetry/cache options.
   * @param sibling_commit_gate Optional RT sibling gate protecting HP commit.
   * @param stabilized_parameters Optional immutable connected-parameter
   * snapshot shared by paired dirty work.
   * @param run GlobalHighPrecision Run owning staging and task leases.
   * @return Mutable target HP output owned by graph.
   * @throws std::bad_alloc unchanged when planning, task, cache, or output
   * storage exhausts memory.
   * @throws GraphError for dirty planning, dispatch, operation, or target
   * validation failures; std::bad_optional_access for an unvalidated request.
   * @note The executor owns phase locking and scheduler lifetime. The optional
   * gate is shared only for the active RealTimeUpdate request. A realtime HP
   * sibling uses its own child Run; no mixed-domain Run is created.
   */
  NodeOutput& compute_high_precision_update(
      GraphModel& graph, compute::RealtimeProxyGraph& proxy_graph,
      const ExecutionStrategy& strategy, const Request& request,
      std::shared_ptr<compute::DirtySiblingCommitGate> sibling_commit_gate =
          nullptr,
      std::shared_ptr<const compute::StabilizedDirtyParameters>
          stabilized_parameters = nullptr,
      compute::ComputeRun* run = nullptr);

  /**
   * @brief Delegates one RT dirty update to RealTimeDirtyExecutor.
   *
   * @param graph Graph supplying topology, parameters, and HP fallback data.
   * @param proxy_graph RT proxy graph receiving the committed result.
   * @param strategy Inline or execution-bound execution policy.
   * @param request Validated dirty request and telemetry/cache options.
   * @param stabilized_parameters Optional immutable parameter preflight output.
   * @param node_synchronization Optional per-node critical sections shared with
   * the concurrent HP sibling of the same RealTimeUpdate transaction.
   * @param run RealTimeUpdate child Run owning task leases and lifecycle state.
   * @return Mutable target RT output owned by proxy_graph.
   * @throws std::bad_alloc unchanged when planning, task, proxy, or output
   * storage exhausts memory.
   * @throws GraphError for dirty planning, dispatch, operation, or target
   * validation failures; std::bad_optional_access for an unvalidated request.
   * @note RT output never becomes formal reusable GraphModel cache state. The
   * optional synchronization owner and Run are retained only for this
   * synchronous call and are not stored in the Graph or runtime.
   */
  NodeOutput& compute_real_time_update(
      GraphModel& graph, compute::RealtimeProxyGraph& proxy_graph,
      const ExecutionStrategy& strategy, const Request& request,
      std::shared_ptr<const compute::StabilizedDirtyParameters>
          stabilized_parameters = nullptr,
      std::shared_ptr<compute::DirtyNodeSynchronization> node_synchronization =
          nullptr,
      compute::ComputeRun* run = nullptr);

  /**
   * @brief Resolves the RT proxy graph for one compute request.
   *
   * @param graph GraphModel used as the inline-store key when no runtime is
   * available.
   * @param strategy Execution strategy that may supply GraphRuntime ownership.
   * @return Runtime-owned proxy graph, or a service-owned inline proxy graph.
   * @throws std::bad_alloc if inline proxy storage must be created.
   * @note The returned proxy is synchronized by dirty executors before
   * planning. Runtime-backed calls never use the inline store.
   */
  compute::RealtimeProxyGraph& realtime_proxy_graph_for(
      GraphModel& graph, const ExecutionStrategy& strategy);

  /**
   * @brief Clears request-scoped timing records under the graph timing mutex.
   *
   * @param graph Graph whose timing collector is reset.
   * @return Nothing.
   * @throws Nothing directly.
   * @note Existing vector capacity may be retained; no timing references may be
   * used concurrently without the same mutex.
   */
  void clear_timing_results(GraphModel& graph);

  /**
   * @brief Executes legacy sequential HP compute after plan inspection.
   *
   * @param graph Graph whose planned HP cone is computed.
   * @param request Target, cache, and telemetry options without scheduler
   * ownership.
   * @param run Request-owned HP descriptor and terminal/storage owner.
   * @return Mutable target HP output owned by graph.
   * @throws std::bad_alloc unchanged when planning, recursion, cache,
   * telemetry, or result storage exhausts memory.
   * @throws GraphError for target, topology, planning, operation, or cache
   * failures.
   * @note The method builds request-local recursion state and releases it after
   * completion; graph-state serialization is owned by the Kernel caller.
   * Sequential execution currently has no staged temporary output, so the Run
   * may safely skip CommitPending.
   */
  NodeOutput& compute_sequential_impl(GraphModel& graph, const Request& request,
                                      compute::ComputeRun& run);

  /**
   * @brief Executes one full execution-bound HP request through its Run.
   *
   * @param graph Graph whose target HP output is computed.
   * @param runtime Runtime providing the GlobalHighPrecision binding and
   * worker-facing observation. The binding may be ownerless for built-in CPU
   * service execution or expose a borrowed legacy scheduler.
   * @param request Full HP target, cache, and telemetry options.
   * @param run Request observer for leased plan, runner, callback, temporary
   * output, exception, and lifecycle state.
   * @return Mutable target HP output owned by graph cache.
   * @throws GraphError for scheduler, planning, execution, cache, or output
   * failures.
   * @throws std::bad_alloc unchanged when scheduler, plan, operation, cache,
   * telemetry, or Run storage exhausts memory.
   * @note Full-HP scheduler callbacks retain stable Run leases and composite
   * task identity. The dispatcher still waits synchronously because graph
   * lifetime and visible commit decoupling remain later work.
   */
  NodeOutput& compute_parallel_hp_impl(GraphModel& graph, GraphRuntime& runtime,
                                       const Request& request,
                                       compute::ComputeRun& run);

  /**
   * @brief Executes an intent-aware request without a GraphRuntime.
   *
   * @param graph Graph whose HP state or service-owned RT proxy is updated.
   * @param request Intent and dirty request to coordinate inline.
   * @param hp_run Request-owned HP Run. Realtime requests supply their HP
   * child.
   * @param rt_run Request-owned RT child Run, or null for
   * GlobalHighPrecision.
   * @return Mutable HP or RT output selected by the intent.
   * @throws std::bad_alloc unchanged when coordination, dirty execution, cache,
   * or output storage exhausts memory.
   * @throws GraphError for intent validation, operation, planning, or output
   * failures.
   * @note Inline RT proxy storage remains owned by ComputeService until service
   * destruction and is protected by inline_rt_proxy_graphs_mutex_. Realtime
   * coordination uses separate child Runs and never creates a mixed-domain
   * Run.
   */
  NodeOutput& compute_with_intent_impl(GraphModel& graph,
                                       const Request& request,
                                       compute::ComputeRun* hp_run,
                                       compute::ComputeRun* rt_run);

  /**
   * @brief Binds intent coordinator callbacks to concrete compute executors.
   *
   * @param graph Graph supplying HP state and topology.
   * @param strategy Inline or execution-bound execution policy.
   * @param request Intent-aware request including any dirty ROI.
   * @param hp_run Request-owned HP Run. Realtime requests supply their HP
   * child.
   * @param rt_run Request-owned RT child Run, or null for
   * GlobalHighPrecision.
   * @return Mutable output selected by IntentUpdateCoordinator.
   * @throws std::bad_alloc unchanged when callback, scheduler, dirty, cache, or
   * output storage exhausts memory.
   * @throws GraphError for invalid intent, missing runtime, planning,
   * operation, scheduler, or output failures.
   * @note Callback captures, the optional sibling gate, and concurrent per-node
   * synchronization are request-local; scheduler and proxy graph ownership
   * remain with strategy/runtime/service. Explicit full HP callbacks reuse the
   * supplied outer Run. Realtime callbacks settle separate HP and RT children.
   */
  NodeOutput& compute_intent_update_impl(GraphModel& graph,
                                         const ExecutionStrategy& strategy,
                                         const Request& request,
                                         compute::ComputeRun* hp_run,
                                         compute::ComputeRun* rt_run);

  /** @brief Borrowed traversal service used by planning and dirty execution. */
  GraphTraversalService& traversal_;
  /** @brief Borrowed disk cache service used by HP execution paths. */
  GraphCacheService& cache_;
  /** @brief Borrowed graph event sink used for request telemetry. */
  GraphEventService& events_;
  /**
   * @brief Borrowed explicitly injected process CPU execution owner.
   *
   * @note Built-in CPU full HP, dirty HP, dirty preflight, and RT work share
   * this fixed process service. It outlives every request-local
   * ComputeService.
   */
  compute::ExecutionService& execution_service_;
  /** @brief Protects service-owned inline RT proxy graph map access. */
  std::mutex inline_rt_proxy_graphs_mutex_;
  /**
   * @brief Service-owned RT proxy graphs for calls without GraphRuntime.
   *
   * @note Keys are borrowed GraphModel addresses and must remain valid while an
   * inline intent request uses the associated entry.
   */
  std::map<GraphModel*, std::unique_ptr<compute::RealtimeProxyGraph>>
      inline_rt_proxy_graphs_;
};

}  // namespace ps
