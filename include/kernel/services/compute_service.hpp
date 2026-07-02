#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph_model.hpp"

namespace ps {

class GraphRuntime;
class GraphTraversalService;
class GraphCacheService;
class GraphEventService;
struct BenchmarkEvent;

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
     * so input-shape changes rebuild tiled task ROIs.
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
   * Request is the public service-layer contract for sequential, parallel,
   * high-precision, and dirty/real-time compute. A missing intent preserves
   * the legacy HP-only path, while a populated intent delegates to
   * IntentUpdateCoordinator with dirty_roi forwarded unchanged.
   *
   * @note The request does not own GraphModel, GraphRuntime, schedulers, or
   * cache entries. All graph mutation happens inside the compute call selected
   * by compute() or compute_parallel().
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
    std::optional<cv::Rect> dirty_roi;
  };

  /**
   * @brief Runtime policy for one ComputeService execution path.
   *
   * ExecutionStrategy captures whether the caller has selected a
   * scheduler-backed path and, when so, which runtime owns the scheduler task
   * pools. It keeps execution policy separate from the semantic request so the
   * same Request can be used by both compute() and compute_parallel().
   *
   * @note A null runtime is valid only when use_parallel_executor is false.
   * Parallel HP or RT scheduling validates the runtime before dispatching work.
   */
  struct ExecutionStrategy {
    /** @brief Runtime that owns scheduler task pools, or null for inline work.
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
   * @throws Nothing directly; referenced services must already be valid.
   * @note ComputeService does not own the supplied services and must not
   * outlive them.
   */
  ComputeService(GraphTraversalService& traversal, GraphCacheService& cache,
                 GraphEventService& events);

  /**
   * @brief Executes a non-parallel compute request against one graph model.
   *
   * @param graph Graph whose node caches, timing, and inspection state are
   * read and mutated.
   * @param request Target, cache, telemetry, intent, and dirty ROI options.
   * @return Mutable output selected by the request, owned by graph node state.
   * @throws GraphError for validation, planning, cache, dispatch, or missing
   * output failures; may propagate operation-specific exceptions.
   * @note A request without intent uses the legacy GlobalHighPrecision
   * recursive path. Intent-aware requests are coordinated inline without a
   * scheduler runtime.
   */
  NodeOutput& compute(GraphModel& graph, const Request& request);

  /**
   * @brief Executes a scheduler-backed compute request against one graph.
   *
   * @param graph Graph whose node caches, timing, and inspection state are
   * read and mutated.
   * @param runtime Runtime that owns intent-specific scheduler task pools.
   * @param request Target, cache, telemetry, intent, and dirty ROI options.
   * @return Mutable output selected by the request, owned by graph node state.
   * @throws GraphError for scheduler lookup, validation, planning, dispatch, or
   * missing output failures; may propagate operation-specific exceptions.
   * @note Dirty RT updates pass HP and RT scheduler task runtimes to
   * IntentUpdateCoordinator when both runtimes are available. Under the current
   * DirectGraphCommit policy the coordinator runs HP then RT sibling callbacks
   * inline, and each dirty callback submits its source-first ready work through
   * the intent-specific scheduler runtime. Kernel callers must enter this
   * method from GraphStateExecutor so scheduler-backed work remains serialized
   * with graph-state operations.
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

  NodeOutput& compute_internal(GraphModel& graph, int node_id,
                               const RecursiveComputeContext& context);

  NodeOutput& compute_high_precision_update(GraphModel& graph,
                                            const ExecutionStrategy& strategy,
                                            const Request& request);

  NodeOutput& compute_real_time_update(GraphModel& graph,
                                       const ExecutionStrategy& strategy,
                                       const Request& request);

  void clear_timing_results(GraphModel& graph);

  NodeOutput& compute_sequential_impl(GraphModel& graph,
                                      const Request& request);

  NodeOutput& compute_with_intent_impl(GraphModel& graph,
                                       const Request& request);

  NodeOutput& compute_intent_update_impl(GraphModel& graph,
                                         const ExecutionStrategy& strategy,
                                         const Request& request);

  GraphTraversalService& traversal_;
  GraphCacheService& cache_;
  GraphEventService& events_;
};

}  // namespace ps
