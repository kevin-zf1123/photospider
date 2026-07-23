#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "compute/compute_commit_policy.hpp"
#include "compute/compute_run.hpp"
#include "compute/compute_supersession.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {

class GraphRuntime;
class GraphTraversalService;
class GraphCacheService;
class GraphEventService;
struct BenchmarkEvent;
struct PreparedIntentUpdateState;
namespace compute {
class DirtyNodeSynchronization;
class DirtySiblingCommitGate;
class ExecutionService;
class RealtimeProxyGraph;
class RunLifecycleAdmissionCandidate;
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
   * @note The request does not own GraphModel, GraphRuntime, workers, routes,
   * or cache entries. Every created Run snapshots graph_identity and qos before
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
     * an optional sequential parallelism cap. Built-in CPU ExecutionService
     * routes apply the explicit class, deadline, and weight without inferring
     * from intent or quality; inline work retains its existing execution
     * behavior.
     */
    compute::ComputeRunQos qos;

    /**
     * @brief Optional private product policy for visible staged publication.
     * @note Kernel supplies one policy tied to its exact request-owned Graph
     * snapshot. Direct ComputeService callers may leave it empty.
     */
    std::shared_ptr<compute::ComputeCommitPolicy> commit_policy;

    /**
     * @brief Optional request-owned RT proxy snapshot used instead of runtime
     * or service-local visible proxy storage.
     * @note Kernel owns this object through complete compute and commit. A
     * non-null pointer is valid only together with that request lifetime.
     */
    compute::RealtimeProxyGraph* staged_realtime_proxy = nullptr;

    /**
     * @brief Optional private current-request cancellation authority.
     * @note Direct backend callers may retain this source and request explicit
     * cancellation. ComputeService creates a request-local source when absent.
     * The wrapper acquires lifecycle leases before attachment so an already
     * requested cancellation remains observable. The field is not installed
     * and does not expand Host, CLI, or IPC v1.
     */
    std::shared_ptr<compute::ComputeRequestCancellationSource>
        cancellation_source;

    /**
     * @brief Optional product-assigned canonical key/generation lineage.
     * @note Kernel supplies this before staging/materialization. Direct private
     * service callers may omit it and receive a local generation-one identity
     * that grants no product supersession authority.
     */
    std::optional<compute::SupersessionIdentity> supersession;
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
     * @note The runtime owns only copied route bindings and execution
     * observations. Every queued route dispatches through the injected process
     * ExecutionService.
     */
    GraphRuntime* runtime = nullptr;

    /** @brief Whether full non-dirty work should use queued route dispatch. */
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
   * outlive them. All queued CPU, GPU-pipeline, and serial-debug paths share
   * the injected execution service; only explicit inline work executes without
   * it.
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
   * output failures, including private cancellation translated to
   * `GraphErrc::ComputeError`; may propagate operation-specific exceptions.
   * @throws std::bad_alloc unchanged when any Host-reachable compute stage
   * exhausts memory.
   * @note A request without intent uses the legacy GlobalHighPrecision
   * recursive path. Intent-aware requests are coordinated inline without a
   * queued execution runtime. Cancellation that wins before the Run commit
   * contender prevents product-visible staged publication; an already entered
   * monolithic provider remains non-preemptible until it returns.
   */
  NodeOutput& compute(GraphModel& graph, const Request& request);

  /**
   * @brief Executes inline work through an already indexed Graph candidate.
   * @param graph Exact staged or direct Graph.
   * @param request Complete service request.
   * @param admission_candidate Candidate begun before revision capture.
   * @return Mutable selected-domain output.
   * @throws The ordinary compute and lifecycle installation errors unchanged.
   * @note Kernel uses this overload so snapshot/revision capture, planning,
   * reservation, Run construction, and staging all occur after the first
   * lifecycle check. The candidate is consumed or rolled back exactly once.
   */
  NodeOutput& compute(
      GraphModel& graph, const Request& request,
      compute::RunLifecycleAdmissionCandidate admission_candidate);

  /**
   * @brief Executes an execution-bound compute request against one graph.
   *
   * @param graph Graph whose node caches, timing, and inspection state are
   * read and mutated.
   * @param runtime Runtime providing Graph lifecycle, intent bindings, and
   * worker-facing observation. Every supported binding uses the injected
   * ExecutionService for physical execution.
   * @param request Target, cache, telemetry, intent, and dirty ROI options.
   * @return Mutable output selected by the request. HP outputs are owned by
   * graph node state; RT dirty outputs are owned by the runtime proxy graph.
   * @throws GraphError for route lookup, validation, planning, dispatch, or
   * missing output failures, including private cancellation translated to
   * `GraphErrc::ComputeError`; may propagate operation-specific exceptions.
   * @throws std::bad_alloc unchanged when planning, task dispatch, operation,
   * cache, telemetry, or result storage exhausts memory.
   * @note Dirty RT updates create separate HP and RT child Runs. Built-in CPU
   * routes submit both children to the fixed process ExecutionService. The
   * staged dirty commit path starts RT before HP, waits for RT proxy commit
   * before HP graph commit, and returns the RT proxy output. Kernel callers
   * must enter this method from GraphStateExecutor so execution-bound work
   * remains serialized with graph-state operations. Cancellation purges only
   * the matching built-in CPU Run's queued work and drains running callbacks;
   * the Kernel product policy prevents visible publication when cancellation
   * wins before commit arbitration.
   */
  NodeOutput& compute_parallel(GraphModel& graph, GraphRuntime& runtime,
                               const Request& request);

  /**
   * @brief Executes route-backed work through an already indexed candidate.
   * @param graph Exact staged Graph.
   * @param runtime Runtime retaining routes, lanes, and lifetime anchor.
   * @param request Complete service request.
   * @param admission_candidate Candidate begun before revision capture.
   * @return Mutable selected-domain output.
   * @throws The ordinary parallel and lifecycle errors unchanged.
   * @note Bundle installation remains all-or-nothing after off-fence staging.
   */
  NodeOutput& compute_parallel(
      GraphModel& graph, GraphRuntime& runtime, const Request& request,
      compute::RunLifecycleAdmissionCandidate admission_candidate);

 private:
  /**
   * @brief Complete off-registry plan for one sequential HP request.
   *
   * @throws std::bad_alloc when plan, operation, or recursion storage grows.
   * @note Operation lookup and all request-sized recursion-map allocation
   * finish before lifecycle installation. The state remains caller-owned until
   * synchronous execution settles.
   */
  struct PreparedSequentialCompute {
    /** @brief Cache-pruned plan retained for diagnostics and execution shape.
     */
    compute::ComputePlan compute_plan;

    /** @brief Planned node order after cache pruning. */
    std::vector<int> execution_order;

    /** @brief Pre-resolved HP operation for every executable planned node. */
    std::unordered_map<int, OpRegistry::OpVariant> resolved_operations;

    /** @brief Pre-reserved recursion/cycle state reused during execution. */
    std::unordered_map<int, bool> visiting;
  };

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

    /** @brief Off-registry operation lookup keyed by planned node id. */
    const std::unordered_map<int, OpRegistry::OpVariant>& resolved_operations;

    /** @brief Retained read-only Run lease observed at recursive boundaries. */
    const compute::ComputeRunLease& run_lease;
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
   * failures, operation failures represented by the backend, and accepted
   * cancellation translated from the retained Run.
   * @note The context and its references must outlive the recursive call tree.
   * Effective parameters and execution-facing node state remain on a
   * request-local Node snapshot. The method commits only the resolved
   * graph-owned input-size hint, HP cache/version, disk-cache effects, and
   * telemetry on the calling thread. Cooperative observations surround
   * recursive dependency resolution, disk cache, provider/tile execution,
   * cache publication, and return; a monolithic provider already entered is
   * non-preemptible.
   */
  NodeOutput& compute_internal(GraphModel& graph, int node_id,
                               const RecursiveComputeContext& context);

  /**
   * @brief Resolves the RT proxy graph for one compute request.
   *
   * @param graph GraphModel used as the inline-store key when no runtime is
   * available.
   * @param strategy Execution strategy that may supply GraphRuntime ownership.
   * @param request Request that may carry a Kernel-owned staged proxy override.
   * @return Request-owned staged proxy, runtime-owned proxy graph, or a
   * service-owned inline proxy graph, in that priority order.
   * @throws std::bad_alloc if inline proxy storage must be created.
   * @note The returned proxy is synchronized by dirty executors before
   * planning. Runtime-backed calls never use the inline store.
   */
  compute::RealtimeProxyGraph& realtime_proxy_graph_for(
      GraphModel& graph, const ExecutionStrategy& strategy,
      const Request& request);

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
   * @brief Plans and resolves one sequential HP request before installation.
   *
   * @param graph Request-local Graph snapshot used for planning.
   * @param request Target, cache, and telemetry controls.
   * @param run_lease Candidate lease used for cancellation observation.
   * @return Complete plan, operations, and reserved recursion state.
   * @throws GraphError for missing targets, invalid topology/plans, or missing
   * operations.
   * @throws std::bad_alloc unchanged from planning and staging.
   * @note The method executes no operation, advances no Run phase, publishes
   * no result, and requires no lifecycle admission.
   */
  PreparedSequentialCompute prepare_sequential_compute(
      GraphModel& graph, const Request& request,
      const compute::ComputeRunLease& run_lease);

  /**
   * @brief Executes legacy sequential HP compute after plan inspection.
   *
   * @param graph Graph whose planned HP cone is computed.
   * @param request Target, cache, and telemetry options without queued route
   * execution.
   * @param prepared Complete plan and operation staging built off-registry.
   * @param run Request-owned HP descriptor and terminal/storage owner.
   * @param run_lease Retained lifecycle lease observed by recursive nodes and
   * tiled-provider boundaries.
   * @return Mutable target HP output owned by graph.
   * @throws std::bad_alloc unchanged when planning, recursion, cache,
   * telemetry, or result storage exhausts memory.
   * @throws GraphError for target, topology, planning, operation, or cache
   * failures.
   * @note The method builds request-local recursion state and releases it after
   * completion. Product Kernel callers supply a request-owned Graph snapshot;
   * the outer Run wrapper advances to CommitPending before visible commit.
   * Cancellation observations suppress later recursive/provider work and the
   * outer commit; monolithic work already executing is non-preemptible.
   */
  NodeOutput& compute_sequential_impl(
      GraphModel& graph, const Request& request,
      PreparedSequentialCompute prepared, compute::ComputeRun& run,
      const compute::ComputeRunLease& run_lease);

  /**
   * @brief Prepares a dirty intent request before lifecycle installation.
   * @param graph Request-local Graph snapshot.
   * @param strategy Inline or route-backed physical policy.
   * @param request Validated GlobalHighPrecision-dirty or RealTimeUpdate
   * request.
   * @param hp_run Candidate HP Run.
   * @param rt_run Candidate RT Run for realtime, otherwise null.
   * @param hp_run_lease Candidate HP lease.
   * @param rt_run_lease Candidate RT lease for realtime, otherwise null.
   * @param sibling_commit_gate Realtime RT-first gate, otherwise null.
   * @return Complete unpublished intent state owning all required dirty phase
   * reservations and ready/index staging.
   * @throws GraphError or standard exceptions from validation, preflight,
   * planning, operation lookup, reservation, and staging.
   * @note Connected-parameter provider results remain request-local during
   * candidate preparation. No physical ready entry, Run phase, lifecycle
   * record, Graph output, or proxy output is published.
   */
  std::unique_ptr<PreparedIntentUpdateState> prepare_intent_update(
      GraphModel& graph, const ExecutionStrategy& strategy,
      const Request& request, compute::ComputeRun* hp_run,
      compute::ComputeRun* rt_run, const compute::ComputeRunLease* hp_run_lease,
      const compute::ComputeRunLease* rt_run_lease,
      std::shared_ptr<compute::DirtySiblingCommitGate> sibling_commit_gate);

  /**
   * @brief Executes one installed prepared dirty intent request.
   * @param prepared Complete unpublished state from prepare_intent_update().
   * @return Mutable selected-domain output.
   * @throws GraphError or standard exceptions from coordination, execution,
   * commit policy, telemetry, or validation.
   * @note The caller must atomically install the matching standalone/group
   * bundle before entry. The preparation is consumed exactly once.
   */
  NodeOutput& execute_prepared_intent_update(
      std::unique_ptr<PreparedIntentUpdateState> prepared);

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
