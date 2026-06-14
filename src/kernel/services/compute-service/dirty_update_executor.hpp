#pragma once

#include <optional>
#include <string>
#include <vector>

#include "benchmark/benchmark_types.hpp"
#include "graph_model.hpp"  // NOLINT(build/include_subdir)
#include "kernel/scheduler/scheduler_task_runtime.hpp"

namespace ps {
class GraphEventService;
class GraphRuntime;
class GraphTraversalService;
}  // namespace ps

namespace ps::compute {

/**
 * @brief Returns a running scheduler task runtime for one compute intent.
 *
 * This helper preserves the ComputeService scheduler contract used by full
 * graph dispatch and dirty ROI dispatch. It looks up the scheduler registered
 * on the supplied GraphRuntime, verifies that it implements
 * SchedulerTaskRuntime, starts it when necessary, and returns the task-runtime
 * facade that accepts concrete ready task callbacks.
 *
 * @param runtime Per-graph runtime that owns intent-specific schedulers.
 * @param intent Compute intent whose scheduler should receive work.
 * @return Running SchedulerTaskRuntime for the requested intent.
 * @throws GraphError when no scheduler is registered or the registered
 * scheduler cannot dispatch planned task callbacks.
 * @note The returned reference remains owned by GraphRuntime. Callers must not
 * store it beyond the active compute request.
 */
SchedulerTaskRuntime& ensure_scheduler_task_runtime(GraphRuntime& runtime,
                                                    ComputeIntent intent);

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
 * @brief Refreshes real-time proxy buffers from committed HP dirty outputs.
 *
 * DownsampleExecutor owns the HP-to-RT refresh that follows high-precision
 * dirty execution when a GraphRuntime is present. Each request records the HP
 * ROI and HP version observed immediately after a node update. Execution skips
 * stale requests, allocates or reuses RT buffers, copies non-image payloads,
 * and records the same downsample/downsample_passthrough events used before
 * the dirty executor split.
 *
 * @note Instances borrow GraphModel, GraphRuntime, and GraphEventService for a
 * single call chain. The graph must remain exclusively owned by the caller for
 * the whole execute() call.
 */
class DownsampleExecutor {
 public:
  /**
   * @brief One pending HP-to-RT refresh request.
   *
   * @note hp_version is compared against the node's current HP and RT versions
   * to avoid overwriting newer RT state with stale downsample work.
   */
  struct Request {
    /** @brief Node whose HP output should refresh the RT proxy. */
    int node_id = -1;

    /** @brief HP-space region that changed during dirty execution. */
    cv::Rect roi_hp;

    /** @brief HP version captured after the dirty node update. */
    int hp_version = 0;
  };

  /**
   * @brief Constructs a downsample executor for one graph-owned update.
   *
   * @param graph Graph containing HP and RT node cache state.
   * @param runtime Optional runtime used only for scheduler trace events.
   * @param events Event service that receives downsample status events.
   * @throws Nothing directly.
   * @note The executor stores borrowed references and performs no ownership
   * transfer.
   */
  DownsampleExecutor(GraphModel& graph, GraphRuntime* runtime,
                     GraphEventService& events);

  /**
   * @brief Executes all pending downsample requests in caller order.
   *
   * @param requests Pending node refreshes created by HP dirty execution.
   * @throws GraphError or OpenCV exceptions if image conversion or resize
   * fails unexpectedly.
   * @note Empty request vectors are valid and leave graph state unchanged.
   */
  void execute(const std::vector<Request>& requests);

 private:
  /**
   * @brief Applies one HP-to-RT refresh request.
   *
   * @param request Request describing the node, ROI, and HP version to copy.
   * @throws GraphError or OpenCV exceptions from buffer conversion or resize.
   * @note Missing nodes, missing HP outputs, and stale generations are skipped
   * to preserve the previous dirty update behavior.
   */
  void execute_one(const Request& request);

  /** @brief Borrowed graph whose node cache state is refreshed. */
  GraphModel& graph_;

  /** @brief Optional runtime used for stale-generation and tile trace events.
   */
  GraphRuntime* runtime_;

  /** @brief Borrowed event sink for downsample status events. */
  GraphEventService& events_;
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
 * @note The executor mutates GraphModel cache and dirty inspection state under
 * the caller's graph ownership rules. It does not change scheduler policy or
 * plugin ABI.
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
   * @note The method preserves the previous lock scope by holding the graph
   * mutex for the duration of planning and execution.
   */
  NodeOutput& execute(GraphModel& graph, GraphRuntime* runtime,
                      const DirtyUpdateRequest& request);

 private:
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
   * @note The method preserves the previous graph mutex lock scope and dirty
   * source-before-downstream ordering.
   */
  NodeOutput& execute(GraphModel& graph, GraphRuntime* runtime,
                      const DirtyUpdateRequest& request);

 private:
  /** @brief Borrowed traversal service for dirty ROI planning. */
  GraphTraversalService& traversal_;

  /** @brief Borrowed event service for RT update status events. */
  GraphEventService& events_;
};

}  // namespace ps::compute
