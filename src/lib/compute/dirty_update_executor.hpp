#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "benchmark/benchmark_types.hpp"
#include "graph_model.hpp"  // NOLINT(build/include_subdir)
#include "kernel/services/compute-service/dirty_execution_common.hpp"

namespace ps {
class GraphEventService;
class GraphRuntime;
class GraphTraversalService;
}  // namespace ps

namespace ps::compute {
class DirtySiblingCommitGate;
class RealtimeProxyGraph;

/**
 * @brief Immutable options for one dirty update executor call.
 *
 * The struct groups the existing ComputeService dirty-update parameters so the
 * HP and RT executors can share a stable call shape without extending the
 * internal ComputeService member surface. Some cache and timing fields are
 * intentionally carried through even though the current dirty ROI
 * implementation does not consume them, because they document the
 * compatibility boundary with the facade entry points.
 *
 * @throws std::bad_alloc when copying cache_precision storage exhausts memory.
 * @note benchmark_events is borrowed for one synchronous execute call;
 * sibling_commit_gate uses shared ownership for coordinated HP/RT lifetime.
 * Copying scalar fields, the borrowed benchmark pointer, and an existing
 * shared_ptr handle does not allocate request-owned storage.
 */
struct DirtyUpdateRequest {
  /** @brief Target node id whose dirty ROI update should produce output. */
  int node_id = -1;

  /** @brief Cache precision label from the internal service request. */
  std::string cache_precision;

  /**
   * @brief Whether this request must ignore existing intent-specific dirty
   * cache.
   *
   * @note HP dirty execution upgrades forced requests to a full-frame HP plan
   * because its staging buffer is not seeded from existing HP output. RT dirty
   * execution keeps RT proxy writes scoped to the RT dirty plan.
   */
  bool force_recache = false;

  /** @brief Whether timing collection was requested by the caller. */
  bool enable_timing = false;

  /** @brief Whether disk cache reads are disabled for this compute request. */
  bool disable_disk_cache = false;

  /** @brief Optional benchmark sink borrowed for the active request only. */
  std::vector<BenchmarkEvent>* benchmark_events = nullptr;

  /** @brief Dirty ROI in high-precision graph coordinates. */
  cv::Rect dirty_roi;

  /**
   * @brief Suppresses direct graph RT downsample writes after HP dirty work.
   *
   * @note RealTimeUpdate HP sibling work sets this flag because the following
   * RT dirty path stages and commits its own output through RealtimeProxyGraph.
   * GlobalHighPrecision dirty ROI may still downsample committed HP output into
   * the runtime-owned RT proxy graph.
   */
  bool suppress_graph_downsample = false;

  /**
   * @brief Optional sibling commit gate for RealTimeUpdate HP work.
   *
   * @note When present, HP dirty execution may compute concurrently but must
   * wait here before mutating GraphModel. The RT sibling marks the gate after
   * committing proxy output; RT failure aborts the HP commit.
   */
  std::shared_ptr<DirtySiblingCommitGate> sibling_commit_gate;
};

/**
 * @brief Executes high-precision dirty ROI updates behind ComputeService.
 *
 * The executor owns the HP dirty update flow that used to live directly in
 * ComputeService: dirty-region planning, compute-task pruning, source-first
 * task submission, tiled or monolithic HP node execution, HP ROI/version
 * commits, and optional HP-to-RT downsample refresh. Forced dirty requests use
 * the target node's current HP extent as the planning ROI so commit installs a
 * complete authoritative HP output. ComputeService remains the facade and
 * delegates one fully validated request at a time.
 *
 * @note Planning/inspection and final validation take graph_mutex_, while
 * scheduler execution runs outside the outer graph lock and relies on
 * request-local node locks for cache writes.
 */
class HighPrecisionDirtyExecutor {
 public:
  /**
   * @brief Constructs an executor that borrows compute support services.
   *
   * @param traversal Traversal service used by DirtyRegionPlanner.
   * @param events Event service used for HP update and downsample events.
   * @throws Nothing directly.
   * @note Both services must outlive the executor call.
   */
  HighPrecisionDirtyExecutor(GraphTraversalService& traversal,
                             GraphEventService& events);

  /**
   * @brief Runs one HP dirty ROI update and returns the target HP output.
   *
   * @param graph Graph whose dirty state and HP caches are updated.
   * @param proxy_graph Runtime-owned RT proxy graph that receives optional
   * GlobalHighPrecision downsample output.
   * @param runtime Optional runtime used to dispatch scheduler tasks and record
   * trace events. A null runtime executes source and downstream work inline.
   * @param request Dirty update options inherited from ComputeService.
   * @return Mutable high-precision target output stored in the graph.
   * @throws GraphError when planning, dependency resolution, operation
   * dispatch, scheduler submission, or target output validation fails.
   * @throws std::bad_alloc unchanged when planning, task, cache, proxy, or
   * output storage exhausts memory.
   * @note The method is phase-split: planning/reset and final validation are
   * serialized with graph_mutex_, but scheduler task execution is not wrapped
   * by the outer graph lock. Forced HP dirty requests must already have a valid
   * target HP extent because they recompute the full frame instead of
   * preserving pixels from the old HP cache.
   */
  NodeOutput& execute(GraphModel& graph, RealtimeProxyGraph& proxy_graph,
                      GraphRuntime* runtime, const DirtyUpdateRequest& request);

 private:
  /**
   * @brief Clears HP cache metadata for nodes selected by one dirty plan.
   *
   * @param graph Graph whose selected HP node state is reset.
   * @param plan HP dirty planner output for the active request.
   * @return Nothing.
   * @throws GraphError when a planned node is missing.
   * @throws std::bad_alloc if graph lookup diagnostic construction exhausts
   * memory.
   * @note Only HP reusable cache, HP ROI, and HP version state are reset.
   */
  void reset_plan_cache(GraphModel& graph,
                        const HighPrecisionDirtyPlan& plan) const;

  /**
   * @brief Validates and returns the target HP output after dirty execution.
   *
   * @param graph Graph containing the target node cache.
   * @param node_id Target node id requested by the internal service caller.
   * @return Mutable high-precision output stored on the target node.
   * @throws GraphError when execution finishes without target HP output.
   * @throws std::bad_alloc if failure diagnostic construction exhausts memory.
   * @note This preserves the previous dirty update failure mode.
   */
  NodeOutput& require_target_output(GraphModel& graph, int node_id) const;

  /** @brief Borrowed traversal service for dirty ROI planning. */
  GraphTraversalService& traversal_;

  /** @brief Borrowed event service for HP update status events. */
  GraphEventService& events_;
};

/**
 * @brief Executes real-time dirty ROI updates behind ComputeService.
 *
 * The executor owns the RT dirty update flow that used to live directly in
 * ComputeService: RT dirty-region planning, compute-task pruning, source-first
 * scheduler submission, RT/HP operation fallback resolution, proxy buffer
 * allocation, tiled or monolithic ROI execution, and RT ROI/version commits.
 *
 * @note RT output is committed to RealtimeProxyGraph and is never promoted to
 * reusable high-precision cache authority or GraphModel node state.
 */
class RealTimeDirtyExecutor {
 public:
  /**
   * @brief Constructs an executor that borrows compute support services.
   *
   * @param traversal Traversal service used by DirtyRegionPlanner.
   * @param events Event service used for RT update events.
   * @throws Nothing directly.
   * @note Both references must remain valid for the executor call.
   */
  RealTimeDirtyExecutor(GraphTraversalService& traversal,
                        GraphEventService& events);

  /**
   * @brief Runs one RT dirty ROI update and returns the target RT output.
   *
   * @param graph Graph used for topology, parameters, and HP fallback output.
   * @param proxy_graph Runtime-owned RT proxy graph receiving staged output.
   * @param runtime Optional runtime used to dispatch scheduler tasks and record
   * trace events. A null runtime executes all work inline.
   * @param request Dirty update options inherited from ComputeService.
   * @return Mutable real-time target output stored in the proxy graph.
   * @throws GraphError when planning, dependency resolution, operation
   * dispatch, scheduler submission, or target output validation fails.
   * @throws std::bad_alloc unchanged when planning, task, proxy, or output
   * storage exhausts memory.
   * @note The method is phase-split: planning/reset and final validation are
   * serialized with graph_mutex_, while dirty source-before-downstream task
   * execution runs outside the outer graph lock.
   */
  NodeOutput& execute(GraphModel& graph, RealtimeProxyGraph& proxy_graph,
                      GraphRuntime* runtime, const DirtyUpdateRequest& request);

 private:
  /**
   * @brief Clears RT cache metadata for nodes selected by one dirty plan.
   *
   * @param proxy_graph Proxy graph whose selected RT node state is reset.
   * @param plan RT dirty planner output for the active request.
   * @return Nothing.
   * @throws std::bad_alloc if reset bookkeeping grows.
   * @note Only proxy output, proxy ROI, proxy version, and RT dirty-source
   * generation metadata are reset.
   */
  void reset_plan_cache(RealtimeProxyGraph& proxy_graph,
                        const RealTimeDirtyPlan& plan) const;

  /**
   * @brief Validates and returns the target RT output after dirty execution.
   *
   * @param proxy_graph Proxy graph containing the target RT output.
   * @param node_id Target node id requested by the internal service caller.
   * @return Mutable real-time output stored on the proxy node.
   * @throws GraphError when execution finishes without target RT output.
   * @throws std::bad_alloc if failure diagnostic construction exhausts memory.
   * @note RT output remains outside GraphModel and is validated separately from
   * HP cache authority.
   */
  NodeOutput& require_target_output(RealtimeProxyGraph& proxy_graph,
                                    int node_id) const;

  /** @brief Borrowed traversal service for dirty ROI planning. */
  GraphTraversalService& traversal_;

  /** @brief Borrowed event service for RT update status events. */
  GraphEventService& events_;
};

}  // namespace ps::compute
