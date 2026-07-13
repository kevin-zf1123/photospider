#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "compute/dirty_region_snapshot.hpp"
#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps {
class GraphModel;
}

namespace ps::compute {

/**
 * @brief Execution shape represented by one planned task graph entry.
 *
 * PlannedTaskKind distinguishes whole-node compatibility work, tiled work, and
 * monolithic operator work after graph expansion. The kind describes planner
 * output only; actual execution resources are selected later by the dispatcher
 * and SchedulerTaskRuntime.
 *
 * @note HP and RT task kinds are interpreted inside their own DirtyDomain and
 * must not be connected across domains.
 */
enum class PlannedTaskKind {
  /** @brief Generic node-level work when no finer task shape is available. */
  Node,
  /** @brief Tile-level work over a concrete output ROI. */
  Tile,
  /** @brief Whole-output monolithic operator work. */
  Monolithic,
};

/**
 * @brief Node-level dependency edge represented in a ComputeTaskGraph.
 *
 * PlannedDependency records the logical relationship between two planned
 * nodes, including dirty ROI mapping when the edge came from a dirty snapshot.
 * Task-level dependency ids are derived from these records after task
 * population.
 *
 * @note The struct is copyable diagnostic data stored in ComputePlan snapshots;
 * it must avoid raw node pointers so inspection remains stable across graph
 * mutation.
 */
struct PlannedDependency {
  /** @brief Upstream node id that must produce data before to_node_id. */
  int from_node_id = -1;
  /** @brief Downstream node id that consumes from_node_id. */
  int to_node_id = -1;
  /** @brief HP or RT domain in which this dependency is valid. */
  DirtyDomain domain = DirtyDomain::HighPrecision;
  /** @brief Input category, currently "image" or "parameter". */
  std::string input_kind = "image";
  /** @brief Upstream ROI represented by this edge, when known. */
  cv::Rect from_roi;
  /** @brief Downstream ROI demanded or affected by this edge, when known. */
  cv::Rect to_roi;
  /** @brief Direction used to explain how dirty ROI mapping was derived. */
  DirtyEdgeDirection direction = DirtyEdgeDirection::BackwardDemand;
};

/**
 * @brief Executable task entry produced by graph planning.
 *
 * PlannedTask is the immutable planning record that later becomes a scheduler
 * closure or dirty work-set member. It contains the node, domain, ROI, tile
 * coordinates, dirty metadata, and upstream task ids required by the dispatcher
 * to submit work in dependency order.
 *
 * @note The task id is local to its ComputeTaskGraph. Dirty fields are updated
 * by DirtySnapshotTaskGraphPruner and do not mutate graph-scoped dirty state.
 */
struct PlannedTask {
  /** @brief Dense id within ComputeTaskGraph::tasks. */
  int task_id = -1;
  /** @brief Graph node id executed by this task. */
  int node_id = -1;
  /** @brief Task execution shape selected by the planner. */
  PlannedTaskKind kind = PlannedTaskKind::Node;
  /** @brief HP or RT domain for this single-domain task. */
  DirtyDomain domain = DirtyDomain::HighPrecision;
  /** @brief Output pixel ROI covered by this task; empty means unknown. */
  cv::Rect output_roi;
  /** @brief Tile x index for tile tasks, or -1 for non-tile work. */
  int tile_x = -1;
  /** @brief Tile y index for tile tasks, or -1 for non-tile work. */
  int tile_y = -1;
  /** @brief Tile side length for tile tasks, or 0 for non-tile work. */
  int tile_size = 0;
  /** @brief Whether the task represents the node's whole output. */
  bool whole_output = false;
  /** @brief Whether this task belongs to a dirty source boundary node. */
  bool source_boundary_eligible = false;
  /** @brief Whether the current dirty snapshot selected this task. */
  bool dirty_selected = false;
  /** @brief Dirty graph generation used when the task was selected. */
  uint64_t dirty_generation = 0;
  /** @brief Upstream task ids that must complete before this task runs. */
  std::vector<int> dependency_task_ids;
};

/**
 * @brief Per-node work summary stored alongside planned tasks.
 *
 * PlannedNodeWork groups task ids, cache state, ROI metadata, and node-level
 * dependency lists for one graph node. It is used for inspection, dirty work
 * materialization, and dispatcher dense-index construction.
 *
 * @note The ROI fields are planning metadata. They do not own image buffers and
 * must not be treated as committed graph output.
 */
struct PlannedNodeWork {
  /** @brief Graph node id represented by this work item. */
  int node_id = -1;
  /** @brief HP or RT domain represented by this work item. */
  DirtyDomain domain = DirtyDomain::HighPrecision;
  /** @brief HP-space ROI represented for inspection or downsample sync. */
  cv::Rect represented_hp_roi;
  /** @brief Domain-local ROI the planner expects to execute. */
  cv::Rect execution_roi;
  /** @brief Whether the work item must recompute the whole output. */
  bool whole_output = false;
  /** @brief Whether a formal reusable HP cache was available during pruning. */
  bool reusable_cache_available = false;
  /** @brief Dirty ROIs associated with this node after snapshot pruning. */
  std::vector<cv::Rect> dirty_rois;
  /** @brief Upstream planned node ids required by this node. */
  std::vector<int> dependency_node_ids;
  /** @brief Downstream planned node ids depending on this node. */
  std::vector<int> dependent_node_ids;
  /** @brief Task ids in ComputeTaskGraph::tasks that belong to this node. */
  std::vector<int> task_ids;
};

/**
 * @brief Task-level graph for one planned compute domain.
 *
 * ComputeTaskGraph stores executable tasks, node-level dependency metadata,
 * and initial task ids derived from dependency_task_ids. It is immutable after
 * scheduler closures are built for a dispatch.
 *
 * @note Runtime dependency counters and ready queues are intentionally absent;
 * those belong to dispatcher submission state.
 */
struct ComputeTaskGraph {
  /** @brief Executable node/tile/monolithic tasks in dense task-id order. */
  std::vector<PlannedTask> tasks;
  /** @brief Planned node dependency edges used to derive task dependencies. */
  std::vector<PlannedDependency> dependencies;
  /** @brief Task ids that have no selected upstream dependencies. */
  std::vector<int> initial_task_ids;
};

/**
 * @brief Dirty snapshot selection result for one update generation.
 *
 * DirtyUpdateWorkSet separates source-boundary tasks from downstream dirty
 * tasks so callers can submit source tasks first and then release dependent
 * dirty work in deterministic order.
 *
 * @note Task ids refer to the ComputePlan passed to
 * DirtySnapshotTaskGraphPruner::materialize().
 */
struct DirtyUpdateWorkSet {
  /** @brief DirtyRegionSnapshot generation used to select the task ids. */
  uint64_t generation = 0;
  /** @brief Dirty source-boundary task ids submitted before downstream work. */
  std::vector<int> dirty_source_task_ids;
  /** @brief Non-source dirty task ids selected for downstream execution. */
  std::vector<int> downstream_task_ids;
};

/**
 * @brief Dirty ROI metadata selected for one node in a generation overlay.
 *
 * DirtyTaskSelectionOverlay keeps these records outside ComputePlan so the
 * node/cache-pruned plan can remain immutable and reusable across repeated ROI
 * updates. The record contains only generation-local ROI overrides required by
 * dirty execution and inspection summaries.
 *
 * @note The ROI values are planning metadata. They do not own image buffers and
 * must not be treated as committed cache state.
 */
struct DirtyNodeSelection {
  /** @brief Graph node id represented by this selected dirty metadata. */
  int node_id = -1;
  /** @brief HP-space ROI represented by the selected dirty task view. */
  cv::Rect represented_hp_roi;
  /** @brief Domain-local ROI selected for execution. */
  cv::Rect execution_roi;
  /** @brief Whether selected dirty work covers the whole output. */
  bool whole_output = false;
  /** @brief Dirty ROIs associated with this node in the snapshot. */
  std::vector<cv::Rect> dirty_rois;
};

/**
 * @brief Generation-local active task view over an immutable ComputePlan.
 *
 * The overlay records which already-expanded PlannedTask ids are active for a
 * dirty snapshot, their task-level dependency ids after snapshot ROI mappings,
 * and the source/downstream work sets used by source-first dirty execution.
 * It avoids copying the full ComputePlan on high-frequency dirty paths.
 *
 * @note Task ids refer to the node/cache-pruned ComputePlan used to create the
 * overlay. The overlay never creates new task shapes.
 */
struct DirtyTaskSelectionOverlay {
  /** @brief Dirty snapshot generation used for this active view. */
  uint64_t generation = 0;
  /** @brief HP or RT domain selected by the parent compute intent. */
  DirtyDomain domain = DirtyDomain::HighPrecision;
  /** @brief Active dirty task ids, including source and downstream work. */
  std::vector<int> active_task_ids;
  /** @brief Dirty source-boundary task ids submitted before downstream work. */
  std::vector<int> dirty_source_task_ids;
  /** @brief Non-source dirty task ids released by task dependencies. */
  std::vector<int> downstream_task_ids;
  /** @brief Initially ready downstream task ids for this active view. */
  std::vector<int> initial_downstream_task_ids;
  /** @brief Active flags aligned with the parent ComputeTaskGraph::tasks. */
  std::vector<bool> active_task_flags;
  /** @brief Source-boundary flags aligned with ComputeTaskGraph::tasks. */
  std::vector<bool> source_boundary_task_flags;
  /** @brief Dependency task ids aligned with ComputeTaskGraph::tasks. */
  std::vector<std::vector<int>> dependency_task_ids;
  /** @brief Node-level dirty ROI overrides keyed by graph node id. */
  std::unordered_map<int, DirtyNodeSelection> node_selections;
  /** @brief Snapshot-aware dependency records used to build the overlay. */
  std::vector<PlannedDependency> dependencies;
};

/**
 * @brief Request attributes used by task graph expansion and pruning.
 *
 * ComputeRequest is the planning-layer description of intent, target node,
 * parallel execution preference, and optional dirty ROI. It is narrower than
 * internal ComputeService request options and contains only data needed to
 * produce a
 * ComputePlan.
 *
 * @note RealTimeUpdate requests are still planned as a single domain per call;
 * HP/RT sibling coordination happens outside this struct.
 */
struct ComputeRequest {
  /** @brief Compute intent whose domain controls task expansion. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  /** @brief Target graph node id requested by the caller. */
  int target_node_id = -1;
  /** @brief Whether the caller intends scheduler-backed execution. */
  bool parallel = false;
  /** @brief Optional dirty ROI used by dirty update callers. */
  std::optional<cv::Rect> dirty_roi;
};

/**
 * @brief Cache-pruned or dirty-pruned plan for one compute request.
 *
 * ComputePlan records the target request, node execution order, per-node work,
 * and task graph used by sequential, parallel, and dirty update execution. It
 * is the stable topology contract for one request while scheduler runtime
 * state is built separately.
 *
 * @note The latest plan remains value-type diagnostic data. Repeated
 * inspection history should store ComputePlanSummary instead of copying every
 * PlannedTask.
 */
struct ComputePlan {
  /** @brief Compute intent whose single-domain task graph was planned. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  /** @brief Target node id from the request. */
  int target_node_id = -1;
  /** @brief Whether the caller intended scheduler-backed execution. */
  bool parallel = false;
  /** @brief Original traversal order for the request target. */
  std::vector<int> execution_order;
  /** @brief Node ids that survived pruning and should be considered planned. */
  std::vector<int> planned_nodes;
  /** @brief Per-node work summaries aligned with planned_nodes by content. */
  std::vector<PlannedNodeWork> planned_work;
  /** @brief Executable task graph derived from planned work. */
  ComputeTaskGraph task_graph;
};

/**
 * @brief Bounded inspection summary for a ComputePlan.
 *
 * ComputePlanSummary stores cheap reader-facing statistics and small task
 * samples instead of copying every PlannedTask into long inspection histories.
 * It may optionally reference a shared immutable full plan when callers need
 * on-demand deep inspection.
 *
 * @note The summary is value-type diagnostic data. Keeping a shared_plan
 * pointer is optional and must not be used by schedulers for runtime state.
 */
struct ComputePlanSummary {
  /** @brief Compute intent represented by the summarized plan. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  /** @brief Target node id from the request. */
  int target_node_id = -1;
  /** @brief Whether the caller intended scheduler-backed execution. */
  bool parallel = false;
  /** @brief Graph topology generation used by full graph cache key. */
  uint64_t topology_generation = 0;
  /** @brief FullTaskGraph cache key used for this plan, when known. */
  std::string full_graph_cache_key;
  /** @brief Number of planned nodes. */
  size_t planned_node_count = 0;
  /** @brief Number of planned tasks. */
  size_t task_count = 0;
  /** @brief Number of tile tasks. */
  size_t tile_task_count = 0;
  /** @brief Number of monolithic tasks. */
  size_t monolithic_task_count = 0;
  /** @brief Number of generic node tasks. */
  size_t node_task_count = 0;
  /** @brief Number of node-level dependency records. */
  size_t dependency_count = 0;
  /** @brief Number of initially ready tasks. */
  size_t initial_task_count = 0;
  /** @brief Number of dirty-overlay active tasks, or task_count without one. */
  size_t active_task_count = 0;
  /** @brief Number of dirty source tasks selected by an overlay. */
  size_t dirty_source_task_count = 0;
  /** @brief Number of downstream dirty tasks selected by an overlay. */
  size_t downstream_task_count = 0;
  /** @brief Number of initially ready downstream dirty tasks. */
  size_t initial_downstream_task_count = 0;
  /** @brief Prefix sample of planned node ids for inspection. */
  std::vector<int> planned_node_sample;
  /** @brief Prefix sample of planned tasks for inspection. */
  std::vector<PlannedTask> task_sample;
  /** @brief Optional shared deep plan reference for on-demand inspection. */
  std::shared_ptr<const ComputePlan> shared_plan;
};

/**
 * @brief Full graph expansion before target/cache/dirty pruning.
 *
 * FullTaskGraph enumerates every node/tile task available for one compute
 * domain. It intentionally does not depend on request target, node cache
 * state, or dirty snapshot.
 *
 * @note Consumers must prune this graph before using it for a specific compute
 * request.
 */
struct FullTaskGraph {
  /** @brief Intent used to choose the single compute domain. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  /** @brief HP or RT domain represented by expanded tasks. */
  DirtyDomain domain = DirtyDomain::HighPrecision;
  /** @brief Node ids included in the full expansion. */
  std::vector<int> expanded_node_ids;
  /** @brief Per-node work summaries before request pruning. */
  std::vector<PlannedNodeWork> expanded_work;
  /** @brief Full task graph before request pruning. */
  ComputeTaskGraph task_graph;
  /**
   * @brief Expanded work index keyed by graph node id.
   *
   * The index is built with the immutable full graph and lets request pruning
   * copy only selected nodes instead of scanning expanded_work on every call.
   */
  std::unordered_map<int, size_t> work_index_by_node;
  /**
   * @brief Full task ids grouped by graph node id.
   *
   * The vectors reference task_graph.tasks ids and are used to copy only the
   * selected task pool during node/cache pruning.
   */
  std::unordered_map<int, std::vector<int>> task_ids_by_node;
  /**
   * @brief Dependency record indices grouped by downstream node id.
   *
   * The vectors reference task_graph.dependencies and let pruning enumerate
   * only candidate edges for selected downstream nodes.
   */
  std::unordered_map<int, std::vector<size_t>> dependency_indices_by_to_node;
};

/**
 * @brief Expands a GraphModel into a full single-domain task graph.
 *
 * @note This boundary does not inspect request target, cache state, or dirty
 * snapshot. It answers only what executable task shapes exist for the graph and
 * domain.
 */
class FullTaskGraphExpander {
 public:
  /**
   * @brief Expands every graph node for the supplied compute intent.
   *
   * @param graph Source graph whose nodes and op metadata are inspected.
   * @param intent Compute intent used to choose HP or RT task domain.
   * @return FullTaskGraph containing all expanded node work and tasks.
   * @throws GraphError or standard exceptions from graph access, extent
   * resolution, op metadata lookup, or allocation.
   */
  FullTaskGraph expand(const GraphModel& graph, ComputeIntent intent) const;
};

/**
 * @brief Prunes a FullTaskGraph to a target/cache-aware request plan.
 *
 * @note Cache hits are recorded as metadata; execution still resolves cache
 * reads and writes through dispatcher/NodeTaskRunner semantics.
 */
class NodeCacheTaskGraphPruner {
 public:
  /**
   * @brief Selects the request target dependency cone from a full graph.
   *
   * @param full_graph Full single-domain task graph to prune.
   * @param request Planning request containing target and intent.
   * @param execution_order Target postorder derived from GraphTraversalService.
   * @param graph GraphModel used to validate nodes and check reusable cache.
   * @return ComputePlan limited to execution_order and selected dependencies.
   * @throws GraphError when requested nodes are missing from graph or full
   * expansion.
   */
  ComputePlan prune(const FullTaskGraph& full_graph,
                    const ComputeRequest& request,
                    const std::vector<int>& execution_order,
                    const GraphModel& graph) const;
};

/**
 * @brief Applies a DirtyRegionSnapshot to a node/cache-pruned plan.
 *
 * DirtySnapshotTaskGraphPruner annotates already-expanded tasks with dirty
 * metadata and materializes source/downstream task id groups. It does not
 * create new task shapes.
 *
 * @note The input plan must already be single-domain and cache-pruned for the
 * caller's HP or RT path.
 */
class DirtySnapshotTaskGraphPruner {
 public:
  /**
   * @brief Selects dirty tasks without copying the node/cache-pruned plan.
   *
   * @param node_cache_plan Immutable plan produced by
   * NodeCacheTaskGraphPruner.
   * @param snapshot Graph-scoped dirty facts for the same compute domain.
   * @param graph Graph used for exact task-level ROI dependencies.
   * @param externally_satisfied_node_ids Optional request-local node identities
   * whose outputs are already staged and must not execute in this phase.
   * @return Generation-local active task overlay and source/downstream groups.
   * @throws std::bad_alloc if overlay vectors or maps cannot grow.
   * @note This is the dirty execution path: it does not mutate or duplicate
   * PlannedTask records, and it preserves task ids from node_cache_plan.
   * Dependencies on explicitly satisfied nodes remain outside the active view
   * and are therefore treated as completed without reusing their task ids.
   */
  DirtyTaskSelectionOverlay select(
      const ComputePlan& node_cache_plan, const DirtyRegionSnapshot& snapshot,
      const GraphModel& graph,
      const std::unordered_set<int>* externally_satisfied_node_ids =
          nullptr) const;

  /**
   * @brief Annotates and clips a node/cache-pruned plan with dirty metadata.
   *
   * @param node_cache_plan Plan produced by NodeCacheTaskGraphPruner.
   * @param snapshot Graph-scoped dirty facts for the same compute domain.
   * @return ComputePlan copy with dirty ROI/work metadata refreshed.
   * @throws std::bad_alloc if copied vectors or maps cannot grow.
   * @note Compatibility and inspection helper only. Dirty execution should use
   * select() so high-frequency ROI updates avoid copying the full plan.
   */
  ComputePlan prune(const ComputePlan& node_cache_plan,
                    const DirtyRegionSnapshot& snapshot,
                    const GraphModel& graph) const;

  /**
   * @brief Materializes source/downstream groups from an active overlay.
   *
   * @param selection Dirty task overlay produced by select().
   * @return DirtyUpdateWorkSet containing source-first and downstream groups.
   * @throws std::bad_alloc if output vectors cannot grow.
   * @note The returned ids preserve task-level granularity and are not folded
   * back to planned nodes.
   */
  DirtyUpdateWorkSet materialize(
      const DirtyTaskSelectionOverlay& selection) const;

  /**
   * @brief Selects source and downstream dirty task ids from a pruned plan.
   *
   * @param plan Dirty-annotated compatibility plan whose tasks are inspected.
   * @param snapshot Dirty snapshot that supplies generation and source nodes.
   * @return DirtyUpdateWorkSet containing source-first and downstream groups.
   * @throws std::bad_alloc if output task id vectors cannot grow.
   * @note This overload exists for tests and legacy inspection. Production
   * dirty execution uses materialize(const DirtyTaskSelectionOverlay&).
   */
  DirtyUpdateWorkSet materialize(const ComputePlan& plan,
                                 const DirtyRegionSnapshot& snapshot) const;
};

/**
 * @brief Computes initially ready task ids for a planned task graph.
 *
 * TaskGraphReadyChecker is a small dependency utility used by dispatcher tests
 * and task submission. It can restrict readiness to an allowed subset for dirty
 * work-set materialization.
 *
 * @note Runtime dependency counters remain in dispatcher submission state; this
 * checker only reads immutable task dependency ids.
 */
class TaskGraphReadyChecker {
 public:
  /**
   * @brief Finds tasks whose dependencies are outside the active subset.
   *
   * @param graph Task graph whose task dependencies are scanned.
   * @param allowed_task_ids Optional subset; when provided, a task is ready if
   * it is in the subset and none of its dependencies are also in the subset.
   * @return Task ids ready for initial submission.
   * @throws std::bad_alloc if temporary allowed or ready vectors grow.
   */
  std::vector<int> initial_ready_task_ids(
      const ComputeTaskGraph& graph,
      const std::vector<int>* allowed_task_ids = nullptr) const;
};

/**
 * @brief Builds the stable cache key for a FullTaskGraph expansion.
 *
 * @param graph Graph whose topology generation participates in the key.
 * @param intent Compute intent whose HP/RT domain is expanded.
 * @return Cache key covering topology generation, intent, task-shape
 * configuration version, and operation-registry task-shape generation.
 * @throws std::bad_alloc if string construction fails.
 * @note The shape config token must change when tile sizing or task shape
 *       selection semantics change. A plugin callback-shape override or unload
 *       advances the registry generation and cannot reuse predecessor tasks.
 */
std::string full_task_graph_cache_key(const GraphModel& graph,
                                      ComputeIntent intent);

/**
 * @brief Returns a cached immutable FullTaskGraph or expands and stores one.
 *
 * @param graph GraphModel owning the per-topology full graph cache.
 * @param intent Compute intent whose single-domain full graph is required.
 * @return Shared immutable full graph for request/cache/dirty pruning.
 * @throws GraphError when the operation registry changes continuously across
 *         all bounded expansion attempts.
 * @throws Standard exceptions from expansion or allocation.
 * @note HP and RT requests use distinct keys and therefore never share task
 *       pools or cross-intent dependencies. Registry generation is sampled
 *       before and after cache lookup and expansion; an inconsistent attempt
 *       is discarded and retried.
 */
std::shared_ptr<const FullTaskGraph> get_or_expand_full_task_graph(
    GraphModel& graph, ComputeIntent intent);

/**
 * @brief Builds a bounded summary for compute plan inspection.
 *
 * @param graph Graph whose topology generation is recorded.
 * @param compute_plan Plan to summarize.
 * @param shared_plan Optional shared deep plan reference.
 * @return Summary containing counts and bounded node/task samples.
 * @throws std::bad_alloc if sample vectors grow.
 * @note Samples are intentionally capped to keep repeated inspection history
 * cheap as tile task graphs grow.
 */
ComputePlanSummary summarize_compute_plan(
    const GraphModel& graph, const ComputePlan& compute_plan,
    const DirtyTaskSelectionOverlay* selection,
    std::shared_ptr<const ComputePlan> shared_plan = nullptr);

/**
 * @brief Builds a bounded summary for compute plan inspection.
 *
 * @param graph Graph whose topology generation is recorded.
 * @param compute_plan Plan to summarize.
 * @param shared_plan Optional shared deep plan reference.
 * @return Summary containing counts and bounded node/task samples.
 * @throws std::bad_alloc if sample vectors grow.
 * @note This overload summarizes an unfiltered plan with no dirty overlay.
 */
ComputePlanSummary summarize_compute_plan(
    const GraphModel& graph, const ComputePlan& compute_plan,
    std::shared_ptr<const ComputePlan> shared_plan = nullptr);

}  // namespace ps::compute
