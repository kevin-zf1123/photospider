#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "benchmark/benchmark_types.hpp"
#include "compute/dirty_execution_common.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {
class GraphEventService;
class GraphRuntime;
class GraphTraversalService;
class SchedulerHostContext;
class SchedulerTaskRuntime;
}  // namespace ps

namespace ps::compute {
class ComputeRun;
class DirtySiblingCommitGate;
class RealtimeProxyGraph;

/**
 * @brief One HP result produced during connected-parameter stabilization.
 *
 * @note The output remains request-local until the HP dirty write buffer
 * commits the complete request. Copying the output preserves any plugin DSO
 * lease carried by its public image/data ownership handles.
 */
struct StabilizedDirtyNodeOutput {
  /** @brief Immutable output produced by the HP preflight execution. */
  NodeOutput output;

  /** @brief HP content version to publish if the complete request succeeds. */
  int hp_version = 0;

  /** @brief Full HP image ROI represented by output, when it has an image. */
  std::optional<PixelRect> hp_roi;
};

/**
 * @brief Immutable connected-parameter snapshot shared by dirty HP/RT paths.
 *
 * A stabilization pass force-executes every HP node required to produce the
 * target cone's connected parameter values. The resulting outputs never enter
 * GraphModel directly. HP phase two imports the complete staged closure into
 * its write buffer, while RT uses only parameter-producing values for
 * effective-parameter resolution and continues to execute image paths in the
 * RT domain.
 *
 * @throws std::bad_alloc when copied output, node-id, or set storage grows.
 * @note Instances are fully built before publication through shared_ptr and
 * are read-only thereafter, so concurrent HP/RT sibling planning may share one
 * exact snapshot without executing a parameter producer twice.
 */
class StabilizedDirtyParameters {
 public:
  /**
   * @brief Returns whether this request has connected parameter producers.
   * @return True when phase-one stabilization executed a producer closure.
   * @throws Nothing.
   */
  bool has_connected_parameters() const noexcept {
    return !parameter_producer_node_ids_.empty();
  }

  /**
   * @brief Returns the generation reserved for both HP and RT siblings.
   * @return Non-zero request generation allocated before preflight.
   * @throws Nothing.
   */
  uint64_t request_generation() const noexcept { return request_generation_; }

  /**
   * @brief Returns the topology generation captured before preflight.
   * @return Graph topology generation shared by both dirty siblings.
   * @throws Nothing.
   */
  uint64_t topology_generation() const noexcept { return topology_generation_; }

  /**
   * @brief Finds any HP output staged by the stabilization closure.
   * @param node_id Graph node id to look up.
   * @return Borrowed immutable output, or nullptr when node_id was not staged.
   * @throws Nothing.
   * @note The returned pointer remains valid for this object's lifetime.
   */
  const NodeOutput* find_staged_output(int node_id) const noexcept;

  /**
   * @brief Finds the exact output of a connected parameter producer.
   * @param node_id Parameter producer node id.
   * @return Borrowed immutable producer output, or nullptr for other nodes.
   * @throws Nothing.
   * @note RT parameter resolution uses this lookup independently from its
   * image-input lookup so HP images cannot replace RT-domain image results.
   */
  const NodeOutput* find_parameter_output(int node_id) const noexcept;

  /**
   * @brief Reports whether a node output geometry may change in this request.
   * @param node_id Graph node id to inspect.
   * @return True for a connected-parameter consumer or one of its image-edge
   * descendants in the target cone.
   * @throws Nothing.
   */
  bool geometry_affected(int node_id) const noexcept;

  /**
   * @brief Returns every node staged by the HP stabilization closure.
   * @return Immutable node-id to staged-output map.
   * @throws Nothing.
   */
  const std::map<int, StabilizedDirtyNodeOutput>& staged_outputs()
      const noexcept {
    return staged_outputs_;
  }

  /**
   * @brief Returns the HP node identities already executed by preflight.
   * @return Immutable set used to exclude HP phase-two tasks explicitly.
   * @throws Nothing.
   */
  const std::unordered_set<int>& staged_node_ids() const noexcept {
    return staged_node_ids_;
  }

  /**
   * @brief Reports whether a staged node is a preflight closure source.
   * @param node_id Node identity to inspect.
   * @return True when no staged upstream node feeds node_id.
   * @throws Nothing.
   */
  bool is_staged_source(int node_id) const noexcept {
    return staged_source_node_ids_.count(node_id) != 0;
  }

  /**
   * @brief Returns direct connected-parameter producer identities.
   * @return Immutable producer node-id set.
   * @throws Nothing.
   */
  const std::unordered_set<int>& parameter_producer_node_ids() const noexcept {
    return parameter_producer_node_ids_;
  }

  /**
   * @brief Returns data-only parameter producers reusable by RT planning.
   * @return Immutable set of producer node ids with no image payload.
   * @throws Nothing.
   * @note Parameter producers that also carry images are not externally
   * satisfied in RT; their image work remains domain-local.
   */
  const std::unordered_set<int>& rt_satisfied_parameter_node_ids()
      const noexcept {
    return rt_satisfied_parameter_node_ids_;
  }

  /**
   * @brief Returns image-carrying parameter producers RT must still execute.
   * @return Immutable set of producer node ids requiring RT-domain work.
   * @throws Nothing.
   * @note Their HP data snapshot remains authoritative for parameter merging;
   * RT execution exists only to preserve image-domain separation.
   */
  const std::unordered_set<int>& rt_required_parameter_node_ids()
      const noexcept {
    return rt_required_parameter_node_ids_;
  }

  /**
   * @brief Returns geometry-affected node identities in the target cone.
   * @return Immutable set used by shadow planning and dirty ROI promotion.
   * @throws Nothing.
   */
  const std::unordered_set<int>& geometry_affected_node_ids() const noexcept {
    return geometry_affected_node_ids_;
  }

 private:
  friend std::shared_ptr<const StabilizedDirtyParameters>
  stabilize_connected_dirty_parameters(GraphModel&, GraphTraversalService&, int,
                                       uint64_t, uint64_t,
                                       SchedulerTaskRuntime*, ExecutionService*,
                                       SchedulerHostContext*, ComputeRun*);

  /** @brief Generation shared by every domain of one dirty request. */
  uint64_t request_generation_ = 0;

  /** @brief Topology identity used to reject mutation during execution. */
  uint64_t topology_generation_ = 0;

  /** @brief HP closure outputs retained until request completion. */
  std::map<int, StabilizedDirtyNodeOutput> staged_outputs_;

  /** @brief Node ids aligned with staged_outputs_ for task exclusion. */
  std::unordered_set<int> staged_node_ids_;

  /** @brief Preflight roots that receive the shared dirty generation. */
  std::unordered_set<int> staged_source_node_ids_;

  /** @brief Direct connected-parameter producer node ids. */
  std::unordered_set<int> parameter_producer_node_ids_;

  /** @brief Data-only producer ids that RT may treat as satisfied. */
  std::unordered_set<int> rt_satisfied_parameter_node_ids_;

  /** @brief Image-carrying producers that retain RT-domain execution. */
  std::unordered_set<int> rt_required_parameter_node_ids_;

  /** @brief Consumers/descendants whose output extent may have changed. */
  std::unordered_set<int> geometry_affected_node_ids_;
};

/**
 * @brief Stabilizes connected parameter values before dirty intent dispatch.
 *
 * The pass finds every connected parameter producer in the target dependency
 * cone, expands their complete upstream closure, and force-executes that
 * closure once in topological HP order into request-local storage. It bypasses
 * memory and disk cache so a producer that consumes a changed image/source
 * observes this request's current operation result.
 *
 * @param graph Live graph whose topology and committed inputs are read.
 * @param traversal Traversal service used to derive target postorder.
 * @param target_node_id Dirty request target.
 * @param request_generation Non-zero identity reserved for the whole request.
 * @param topology_generation Topology identity captured with the generation.
 * @param task_runtime Optional HP scheduler runtime. When supplied, every
 * preflight node is opened as a high-priority single-handle initial batch and
 * its matching completion wait drains before the next topological node.
 * @param execution_service Optional process CPU service used instead of
 * task_runtime.
 * @param host Optional service trace target; required with execution_service.
 * @param run Optional HP child Run whose leases name owned preflight work;
 * required with execution_service.
 * @return Immutable shared request snapshot. An empty producer set means no
 * connected-parameter preflight was needed, but generation identity remains.
 * @throws GraphError for missing targets, dependencies, operations, or empty
 * operation results.
 * @throws std::bad_alloc unchanged from topology, operation, or output storage.
 * @note The caller must serialize graph topology/cache mutation for the call.
 * No GraphModel cache, RT proxy, timing, or event state is published. Operation
 * callbacks retain the existing non-rollbackable external side-effect
 * semantics, but each preflight closure node executes at most once and is not
 * repeated by HP phase two.
 */
std::shared_ptr<const StabilizedDirtyParameters>
stabilize_connected_dirty_parameters(
    GraphModel& graph, GraphTraversalService& traversal, int target_node_id,
    uint64_t request_generation, uint64_t topology_generation,
    SchedulerTaskRuntime* task_runtime = nullptr,
    ExecutionService* execution_service = nullptr,
    SchedulerHostContext* host = nullptr, ComputeRun* run = nullptr);

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
 * sibling_commit_gate and node_synchronization use shared ownership for the
 * coordinated HP/RT lifetime. Copying scalar fields, the borrowed benchmark
 * pointer, and existing shared_ptr handles does not allocate request-owned
 * storage.
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
  PixelRect dirty_roi;

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

  /**
   * @brief Optional exact connected-parameter snapshot shared by HP and RT.
   *
   * @note The snapshot is immutable. HP imports its staged closure into the
   * final HP write buffer; RT reads only exact parameter producer values and
   * continues to execute image-path work in its own domain.
   */
  std::shared_ptr<const StabilizedDirtyParameters> stabilized_parameters;

  /**
   * @brief Optional per-node critical sections shared by HP/RT siblings.
   *
   * @note `ComputeService` supplies one transaction-scoped owner only when a
   * scheduler-backed RealTimeUpdate will run HP and RT concurrently. Each
   * executor creates an independent local owner when this pointer is null.
   * Shared ownership lasts through sibling failure cleanup and scheduler drain;
   * the object is never retained by a Graph or process-wide service.
   */
  std::shared_ptr<DirtyNodeSynchronization> node_synchronization;
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
 * transaction-local per-node critical sections. Concurrent HP/RT siblings
 * share those sections only for the same RealTimeUpdate transaction.
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
   * @param run Optional request-owned HP Run. GlobalHighPrecision and
   * scheduler-backed realtime HP children supply it so staging and service
   * submissions remain Run-scoped.
   * @param execution_service Optional process CPU service selected by the
   * runtime route. It requires non-null runtime and run.
   * @return Mutable high-precision target output stored in the graph.
   * @throws GraphError when planning, dependency resolution, operation
   * dispatch, scheduler submission, or target output validation fails.
   * @throws std::bad_alloc unchanged when planning, task, cache, proxy, or
   * output storage exhausts memory.
   * @note The method is phase-split: planning/reset and final validation are
   * serialized with graph_mutex_, but scheduler task execution is not wrapped
   * by the outer graph lock. Forced HP dirty requests must already have a valid
   * target HP extent because they recompute the full frame instead of
   * preserving pixels from the old HP cache. A supplied Run receives exactly
   * one staging buffer and the applicable queued/running/commit phases.
   */
  NodeOutput& execute(GraphModel& graph, RealtimeProxyGraph& proxy_graph,
                      GraphRuntime* runtime, const DirtyUpdateRequest& request,
                      ComputeRun* run = nullptr,
                      ExecutionService* execution_service = nullptr);

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
   * @param run Optional RT child Run required for process-service routing.
   * @param execution_service Optional process CPU service selected by the
   * runtime route.
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
                      GraphRuntime* runtime, const DirtyUpdateRequest& request,
                      ComputeRun* run = nullptr,
                      ExecutionService* execution_service = nullptr);

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
