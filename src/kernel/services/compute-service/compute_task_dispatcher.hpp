#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "benchmark/benchmark_types.hpp"
#include "graph_model.hpp"
#include "kernel/scheduler/scheduler_task_runtime.hpp"

namespace ps {
class GraphTraversalService;
class GraphCacheService;
class GraphEventService;
}  // namespace ps

namespace ps::compute {

/**
 * @brief Dispatches the GlobalHighPrecision compute task graph through a
 * SchedulerTaskRuntime.
 *
 * ComputeTaskDispatcher owns the execution bridge between the compute-service
 * planning layer and the scheduler runtime. For one request it builds the
 * cache-pruned task graph, materializes scheduler closures, releases
 * dependency-ready work, collects worker outputs in temporary slots, and
 * serializes final GraphModel cache mutation after the scheduler drains.
 *
 * The dispatcher does not choose scheduling policy. Thread ownership, worker
 * queues, task prioritization, and exception capture remain responsibilities
 * of the supplied SchedulerTaskRuntime. It also does not implement the
 * real-time dirty-region planner; dirty update callers only reuse the static
 * source-first submission helper below.
 *
 * @note Instances borrow traversal, cache, and event services. Those services
 * must outlive the dispatcher and must refer to the same graph runtime context
 * used by the caller.
 */
class ComputeTaskDispatcher {
 public:
  /**
   * @brief Sequential execution callback used when planning produces no
   * scheduler tasks.
   *
   * @param graph Graph being computed.
   * @param node_id Target node that still needs a high-precision output.
   * @param allow_disk_cache Whether the fallback may read disk cache entries.
   * @return Mutable high-precision output owned by the graph.
   * @throws GraphError or operation-specific exceptions from the fallback
   * implementation.
   * @note The callback is invoked synchronously on the caller thread and is
   * expected to preserve ComputeService::compute_internal cache semantics.
   */
  using SequentialFallback =
      std::function<NodeOutput&(GraphModel&, int, bool allow_disk_cache)>;

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
   * high-precision caches, builds scheduler tasks, waits for scheduler
   * completion, commits temporary outputs into GraphModel under the graph
   * mutex, writes configured disk caches, and returns the target output.
   *
   * @param graph Graph whose nodes and runtime cache state are computed.
   * @param task_runtime Scheduler runtime that receives initial and
   * dependency-ready tasks.
   * @param request Immutable execution options for this dispatch.
   * @param sequential_fallback Synchronous fallback used when no scheduler task
   * graph is produced but the target still needs output.
   * @return Mutable target high-precision output stored in the graph.
   * @throws GraphError when the node is missing, no operation exists, a
   * dependency is unavailable, scheduling fails, or dispatch finishes without
   * target output. It may also rethrow operation, OpenCV, cache, or fallback
   * exceptions wrapped with compute-stage context.
   * @note Worker tasks do not mutate GraphModel caches directly. They publish
   * temporary results, and execute() serializes final cache ownership after the
   * scheduler drains.
   */
  NodeOutput& execute(GraphModel& graph, SchedulerTaskRuntime& task_runtime,
                      const ComputeDispatchRequest& request,
                      SequentialFallback sequential_fallback);

  /**
   * @brief Runs dirty-source tasks before downstream dirty-update tasks.
   *
   * Source tasks are submitted with high priority and may run concurrently.
   * After all source tasks finish, before_downstream is invoked on the caller
   * thread when provided. Downstream tasks are then submitted with normal
   * priority and waited in vector order to preserve deterministic commit
   * ordering for dirty update call sites.
   *
   * @param task_runtime Scheduler runtime used by the dirty-update caller.
   * @param source_tasks Dirty source task closures to run first.
   * @param downstream_tasks Dependent dirty-update task closures to run after
   * all sources and the optional boundary check complete.
   * @param epoch Optional scheduler epoch forwarded to submitted tasks.
   * @param before_downstream Optional callback for source-boundary validation.
   * @throws Rethrows any task or callback exception after recording it in the
   * scheduler runtime.
   * @note This helper is shared by high-precision dirty ROI and real-time dirty
   * update paths; it does not build or prune the dirty task graph itself.
   */
  static void submit_dirty_ready_tasks_source_first(
      SchedulerTaskRuntime& task_runtime,
      std::vector<SchedulerTaskRuntime::Task>&& source_tasks,
      std::vector<SchedulerTaskRuntime::Task>&& downstream_tasks,
      std::optional<uint64_t> epoch = std::nullopt,
      std::function<void()> before_downstream = nullptr);

 private:
  /**
   * @brief Clears graph timing state before a timed dispatch.
   *
   * @param graph Graph whose TimingCollector is reset.
   * @throws Nothing directly.
   * @note Holds graph.timing_mutex_ while mutating timing fields.
   */
  static void clear_timing_results(GraphModel& graph);

  /** @brief Borrowed traversal service used to build target execution order. */
  GraphTraversalService& traversal_;

  /** @brief Borrowed cache service used for disk cache load and save steps. */
  GraphCacheService& cache_;

  /** @brief Borrowed event service used for per-node compute status events. */
  GraphEventService& events_;
};

}  // namespace ps::compute
