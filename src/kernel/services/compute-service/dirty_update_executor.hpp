#pragma once

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

/**
 * @brief Immutable options for one dirty update executor call.
 *
 * The struct groups the existing ComputeService dirty-update parameters so the
 * HP and RT executors can share a stable call shape without extending the
 * public ComputeService API. Some cache and timing fields are intentionally
 * carried through even though the current dirty ROI implementation does not
 * consume them, because they document the compatibility boundary with the
 * facade entry points.
 */
struct DirtyUpdateRequest {
  /** @brief Target node id whose dirty ROI update should produce output. */
  int node_id = -1;

  /** @brief Cache precision label from the public compute entry point. */
  std::string cache_precision;

  /** @brief Whether existing intent-specific dirty caches should be cleared. */
  bool force_recache = false;

  /** @brief Whether timing collection was requested by the caller. */
  bool enable_timing = false;

  /** @brief Whether disk cache reads are disabled for this compute request. */
  bool disable_disk_cache = false;

  /** @brief Optional benchmark sink borrowed for the active request only. */
  std::vector<BenchmarkEvent>* benchmark_events = nullptr;

  /** @brief Dirty ROI in high-precision graph coordinates. */
  cv::Rect dirty_roi;
};

/**
 * @brief Executes high-precision dirty ROI updates behind ComputeService.
 *
 * The executor owns the HP dirty update flow that used to live directly in
 * ComputeService: dirty-region planning, compute-task pruning, source-first
 * task submission, tiled or monolithic HP node execution, HP ROI/version
 * commits, and optional HP-to-RT downsample refresh. ComputeService remains the
 * facade and delegates one fully validated request at a time.
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
   * @param runtime Optional runtime used to dispatch scheduler tasks and record
   * trace events. A null runtime executes source and downstream work inline.
   * @param request Dirty update options inherited from ComputeService.
   * @return Mutable high-precision target output stored in the graph.
   * @throws GraphError when planning, dependency resolution, operation
   * dispatch, scheduler submission, or target output validation fails.
   * @note The method is phase-split: planning/reset and final validation are
   * serialized with graph_mutex_, but scheduler task execution is not wrapped
   * by the outer graph lock.
   */
  NodeOutput& execute(GraphModel& graph, GraphRuntime* runtime,
                      const DirtyUpdateRequest& request);

 private:
  /**
   * @brief Clears HP cache metadata for nodes selected by one dirty plan.
   *
   * @param graph Graph whose selected HP node state is reset.
   * @param plan HP dirty planner output for the active request.
   * @throws GraphError when a planned node is missing.
   * @note Only HP reusable cache, HP ROI, and HP version state are reset.
   */
  void reset_plan_cache(GraphModel& graph,
                        const HighPrecisionDirtyPlan& plan) const;

  /**
   * @brief Validates and returns the target HP output after dirty execution.
   *
   * @param graph Graph containing the target node cache.
   * @param node_id Target node id requested by the public facade.
   * @return Mutable high-precision output stored on the target node.
   * @throws GraphError when execution finishes without target HP output.
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
 * @note RT output remains transient interactive state and is never promoted to
 * reusable high-precision cache authority.
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
   * @param graph Graph whose dirty state and RT caches are updated.
   * @param runtime Optional runtime used to dispatch scheduler tasks and record
   * trace events. A null runtime executes all work inline.
   * @param request Dirty update options inherited from ComputeService.
   * @return Mutable real-time target output stored in the graph.
   * @throws GraphError when planning, dependency resolution, operation
   * dispatch, scheduler submission, or target output validation fails.
   * @note The method is phase-split: planning/reset and final validation are
   * serialized with graph_mutex_, while dirty source-before-downstream task
   * execution runs outside the outer graph lock.
   */
  NodeOutput& execute(GraphModel& graph, GraphRuntime* runtime,
                      const DirtyUpdateRequest& request);

 private:
  /**
   * @brief Clears RT cache metadata for nodes selected by one dirty plan.
   *
   * @param graph Graph whose selected RT node state is reset.
   * @param plan RT dirty planner output for the active request.
   * @throws GraphError when a planned node is missing.
   * @note Only transient RT cache, RT ROI, and RT version state are reset.
   */
  void reset_plan_cache(GraphModel& graph, const RealTimeDirtyPlan& plan) const;

  /**
   * @brief Validates and returns the target RT output after dirty execution.
   *
   * @param graph Graph containing the target node cache.
   * @param node_id Target node id requested by the public facade.
   * @return Mutable real-time output stored on the target node.
   * @throws GraphError when execution finishes without target RT output.
   * @note RT output remains transient state and is validated separately from HP
   * cache authority.
   */
  NodeOutput& require_target_output(GraphModel& graph, int node_id) const;

  /** @brief Borrowed traversal service for dirty ROI planning. */
  GraphTraversalService& traversal_;

  /** @brief Borrowed event service for RT update status events. */
  GraphEventService& events_;
};

}  // namespace ps::compute
