#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "benchmark/benchmark_types.hpp"
#include "execution/execution_task_runtime.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {
class GraphTraversalService;
class GraphCacheService;
class GraphEventService;
class ExecutionHostContext;
}  // namespace ps

namespace ps::compute {
class ComputeRun;
class ComputeRunLease;
class ExecutionService;
struct PreparedComputeDispatchState;

/**
 * @brief Move-only unpublished full-HP plan and physical batch.
 *
 * The preparation owns a copied lifecycle lease, the Run-owned plan's stable
 * address, the fully reserved ExecutionService batch, and all route/commit
 * inputs needed after lifecycle installation. It publishes no ready entry or
 * Graph result before execute_prepared() consumes it.
 *
 * @throws Nothing from movement and destruction.
 * @note Destruction before execution rolls back the physical reservation and
 * staged ready/index nodes. The value grants no registry-admission authority.
 */
class PreparedComputeDispatch final {
 public:
  /** @brief Creates an inactive moved-from preparation. */
  PreparedComputeDispatch() noexcept = default;

  /**
   * @brief Transfers unpublished dispatch ownership.
   * @param other Preparation made inactive.
   * @throws Nothing.
   */
  PreparedComputeDispatch(PreparedComputeDispatch&& other) noexcept;

  /**
   * @brief Replaces only an inactive preparation by transfer.
   * @param other Preparation made inactive.
   * @return Reference to this preparation.
   * @throws Nothing; replacing active ownership terminates.
   */
  PreparedComputeDispatch& operator=(PreparedComputeDispatch&& other) noexcept;

  /**
   * @brief Releases any unpublished physical batch.
   * @throws Nothing; trusted cleanup failure terminates.
   */
  ~PreparedComputeDispatch() noexcept;

  /** @brief Prevents duplicate publication and reservation ownership. */
  PreparedComputeDispatch(const PreparedComputeDispatch&) = delete;

  /** @brief Prevents duplicate publication and reservation assignment. */
  PreparedComputeDispatch& operator=(const PreparedComputeDispatch&) = delete;

  /**
   * @brief Reports whether this value still owns a prepared dispatch.
   * @return True until movement or execute_prepared().
   * @throws Nothing.
   */
  bool active() const noexcept { return state_ != nullptr; }

 private:
  friend class ComputeTaskDispatcher;

  /**
   * @brief Owns one fully prepared dispatcher state.
   * @param state Complete unpublished state.
   * @throws Nothing.
   */
  explicit PreparedComputeDispatch(
      std::unique_ptr<PreparedComputeDispatchState> state) noexcept;

  /** @brief Complete unpublished dispatcher and physical batch ownership. */
  std::unique_ptr<PreparedComputeDispatchState> state_;
};

/**
 * @brief Dispatches the GlobalHighPrecision compute task graph through a
 * ExecutionTaskRuntime.
 *
 * ComputeTaskDispatcher owns the execution bridge between the compute-service
 * planning layer and the execution runtime. For one request it asks the
 * caller-owned ComputeRun to build and retain the cache-pruned task graph and
 * owned runner, acquires a dispatcher/commit lease, releases dependency-ready
 * work as runtime-owned callbacks with composite Run-local identity,
 * collects worker outputs in Run-owned temporary slots, and serializes final
 * GraphModel cache mutation after execution drains.
 *
 * The dispatcher does not choose scheduling policy. Thread ownership, worker
 * queues, task prioritization, and exception capture remain responsibilities
 * of the supplied ExecutionTaskRuntime. It also does not implement the
 * real-time dirty-region planner; dirty update callers only reuse the static
 * source-first submission helper below.
 *
 * Its C++ `public:` members are callable only inside the private backend source
 * tree. That language-access label does not make them product Host API,
 * frontend seam, or plugin ABI; external callers use `ps::Host`.
 *
 * @note Instances borrow traversal, cache, and event services. Those services
 * must outlive the dispatcher and must refer to the same graph runtime context
 * used by the caller.
 */
class ComputeTaskDispatcher {
 public:
  /**
   * @brief Immutable per-call options for one high-precision dispatch.
   *
   * The request groups cache, timing, and benchmark knobs so helper objects
   * share one execution contract without a long positional argument list.
   * Pointers inside the request remain borrowed; the dispatcher never stores
   * them beyond the active execute() call.
   */
  struct ComputeDispatchRequest {
    /** @brief Target node id to compute; must exist in the supplied graph. */
    int node_id = -1;

    /** @brief Cache precision label used when committing disk cache entries. */
    std::string cache_precision;

    /**
     * @brief Forces planned high-precision memory caches to be cleared before
     * dispatch.
     */
    bool force_recache = false;

    /** @brief Enables timing collection into graph timing and benchmark logs.
     */
    bool enable_timing = false;

    /** @brief Disables disk cache reads while preserving in-memory results. */
    bool disable_disk_cache = false;

    /**
     * @brief Optional benchmark sink appended under the graph timing mutex.
     *
     * @note The pointed vector must remain alive for the duration of execute().
     */
    std::vector<BenchmarkEvent>* benchmark_events = nullptr;
  };

  /**
   * @brief Constructs a dispatcher that borrows compute support services.
   *
   * @param traversal Traversal service used to derive the target postorder.
   * @param cache Cache service used for disk reads and serialized writes.
   * @param events Event service used to publish per-node compute events.
   * @throws Nothing directly; references must already be valid.
   * @note The dispatcher stores references and performs no ownership transfer.
   */
  ComputeTaskDispatcher(GraphTraversalService& traversal,
                        GraphCacheService& cache, GraphEventService& events);

  /**
   * @brief Executes one cache-pruned GlobalHighPrecision plan.
   *
   * The method validates the target node, optionally clears timing and planned
   * high-precision caches, builds executable tasks, waits for runtime
   * completion, commits temporary outputs into GraphModel under the graph
   * mutex, writes configured disk caches, and returns the target output.
   *
   * @param graph Graph whose nodes and runtime cache state are computed.
   * @param task_runtime Execution runtime that receives initial and
   * dependency-ready tasks.
   * @param request Immutable execution options for this dispatch.
   * @param run Request observer that retains the plan and mints leases for
   * dispatcher, owned runner, callback, exception, and temporary-output state.
   * @param lifecycle_lease Retained request lease observed and copied into
   * dispatcher/callback ownership.
   * @return Mutable target high-precision output stored in the graph.
   * @throws GraphError when the node is missing, no operation exists, a
   * dependency is unavailable, scheduling fails, or dispatch finishes without
   * target output. It may also rethrow operation, OpenCV, or cache exceptions
   * wrapped with compute-stage context.
   * @throws std::bad_alloc unchanged when plan, task, operation, cache,
   * telemetry, or result storage exhausts memory.
   * @note Worker tasks do not mutate GraphModel caches directly. They publish
   * temporary results, and execute() serializes final cache ownership after the
   * runtime drains. Cancellation is observed before planning, publication,
   * phase changes, result commit, and return; cancellation observed before
   * those boundaries suppresses dependent publication and final Graph cache
   * commit. A monolithic provider already entered is non-preemptible, while
   * tiled providers observe between tiles. Full-HP callbacks own Run leases and
   * carry
   * `(ComputeRunId, ComputeRunLocalTaskId)` identity; they contain no borrowed
   * ExecutionTaskExecutor pointer. The current graph/runtime lifetime and
   * visible commit still require synchronous wait.
   */
  NodeOutput& execute(GraphModel& graph, ExecutionTaskRuntime& task_runtime,
                      const ComputeDispatchRequest& request, ComputeRun& run,
                      const ComputeRunLease& lifecycle_lease);

  /**
   * @brief Executes one full-HP plan through the injected CPU service.
   *
   * @param graph Graph whose temporary outputs are committed after settlement.
   * @param execution_service Process-owned multi-Run CPU execution domain.
   * @param host Active Graph observation context for worker trace forwarding.
   * @param request Immutable full-HP dispatch options.
   * @param run Request owner retaining plan, runner, results, and leases.
   * @param lifecycle_lease Retained request lease observed and copied into
   * service submissions.
   * @return Mutable target high-precision output stored in Graph state.
   * @throws GraphError for planning, operation, service, cache, or output
   * validation failures.
   * @throws std::bad_alloc unchanged from planning, submission, or commit.
   * @throws The exact first service worker exception after batch settlement.
   * @note Only ready submissions cross into execution_service. This overload
   * retains all planning, dependency, result, and commit authority in the
   * dispatcher/Run boundary. Accepted cancellation retires this Run's queued
   * service entries and drains callbacks already running; cancellation
   * observed before publication suppresses dependent submission and final
   * Graph cache commit.
   */
  NodeOutput& execute(GraphModel& graph, ExecutionService& execution_service,
                      ExecutionHostContext& host,
                      const std::string& execution_type,
                      const ComputeDispatchRequest& request, ComputeRun& run,
                      const ComputeRunLease& lifecycle_lease);

  /**
   * @brief Prepares one service-backed full-HP dispatch off-registry.
   *
   * @param graph Graph whose captured plan and temporary output state are
   * retained by run.
   * @param execution_service Process execution owner that reserves the whole
   * physical batch without publishing it.
   * @param host Active route/trace context retained through synchronous
   * execution.
   * @param execution_type Immutable copied private route identity.
   * @param request Full-HP cache and telemetry options.
   * @param run Candidate Run retaining the plan and task runner.
   * @param lifecycle_lease Candidate lease retained by callbacks and staging.
   * @return Move-only complete unpublished dispatch.
   * @throws GraphError or standard exceptions from validation, planning,
   * operation resolution, resource estimation/reservation, and staging.
   * @throws std::bad_alloc unchanged from any off-registry allocation.
   * @note The method may mutate only request-local Graph planning/cache state.
   * It does not advance Run phase, publish a ready entry, execute an operation,
   * commit a result, or install lifecycle admission.
   */
  PreparedComputeDispatch prepare(GraphModel& graph,
                                  ExecutionService& execution_service,
                                  ExecutionHostContext& host,
                                  const std::string& execution_type,
                                  const ComputeDispatchRequest& request,
                                  ComputeRun& run,
                                  const ComputeRunLease& lifecycle_lease);

  /**
   * @brief Publishes, executes, and commits one installed prepared dispatch.
   *
   * @param prepared Active local preparation returned by prepare().
   * @return Mutable target high-precision output committed to graph.
   * @throws GraphError or standard exceptions from cancellation, execution,
   * cache commit, telemetry, or target validation.
   * @note The caller must atomically install the matching lifecycle bundle
   * before entry. This method consumes publication ownership exactly once.
   */
  NodeOutput& execute_prepared(PreparedComputeDispatch prepared);

  /**
   * @brief Runs dirty task handles with source-before-downstream ordering.
   *
   * Source handles are submitted as the first dirty dependency batch with high
   * priority and waited to completion. The optional before_downstream callback
   * then validates the source boundary on the caller thread. Finally, initial
   * downstream handles are submitted as a normal-priority dependency batch and
   * waited until runtime dependency release drains the full downstream count.
   *
   * @param task_runtime Execution runtime used by the dirty-update caller.
   * @param source_handles Ready dirty source task handles to submit first.
   * @param source_task_count Total source task count tracked by the runtime.
   * @param initial_downstream_handles Initial downstream handles whose
   * dependencies are already satisfied after source completion.
   * @param downstream_task_count Total downstream dirty task count tracked by
   * the runtime.
   * @param before_downstream Optional callback for source-boundary validation.
   * @return Nothing.
   * @throws Rethrows any runtime, task, or callback exception.
   * @throws std::bad_alloc unchanged when handle submission, dependency, or
   * callback storage exhausts memory.
   * @note Production HP and RT dirty executors use this overload so
   * source-first runtime submission remains a ComputeTaskDispatcher boundary
   * while dirty executors own only plan-specific ExecutionTaskExecutor
   * construction.
   */
  static void submit_dirty_ready_tasks_source_first(
      ExecutionTaskRuntime& task_runtime,
      std::vector<ExecutionTaskHandle>&& source_handles, int source_task_count,
      std::vector<ExecutionTaskHandle>&& initial_downstream_handles,
      int downstream_task_count,
      std::function<void()> before_downstream = nullptr);

 private:
  /**
   * @brief Clears graph timing state before a timed dispatch.
   *
   * @param graph Graph whose TimingCollector is reset.
   * @return Nothing.
   * @throws Nothing directly.
   * @note Holds graph.timing_mutex_ while mutating timing fields.
   */
  static void clear_timing_results(GraphModel& graph);

  /**
   * @brief Shares full-HP planning, runner setup, and commit across routes.
   *
   * @param graph Graph whose plan and final cache state are mutated.
   * @param task_runtime Injected ready-submission runtime
   * used by the Run-owned node runner and completion routes.
   * @param execution_service Non-null only for the migrated CPU service route.
   * @param host Non-null active trace target only for the service route.
   * @param request Immutable dispatch controls.
   * @param run Request owner retaining all asynchronous state.
   * @param lifecycle_lease Retained request lease observed and copied into the
   * selected physical route.
   * @return Mutable committed target output.
   * @throws GraphError or standard exceptions from planning, execution,
   * service/runtime settlement, cache, telemetry, or commit.
   * @note The optional route pointers select only physical dispatch; every
   * semantic planning and visible commit stage is shared. Cooperative
   * observations bracket planning, dispatch, phase transitions, and final
   * result commit so cancellation that wins before commit leaves temporary
   * outputs unpublished.
   */
  NodeOutput& execute_impl(
      GraphModel& graph, ExecutionTaskRuntime& task_runtime,
      ExecutionService* execution_service, ExecutionHostContext* host,
      const std::string* execution_type, const ComputeDispatchRequest& request,
      ComputeRun& run, const ComputeRunLease& lifecycle_lease);

  /** @brief Borrowed traversal service used to build target execution order. */
  GraphTraversalService& traversal_;

  /** @brief Borrowed cache service used for disk cache load and save steps. */
  GraphCacheService& cache_;

  /** @brief Borrowed event service used for per-node compute status events. */
  GraphEventService& events_;
};

}  // namespace ps::compute
