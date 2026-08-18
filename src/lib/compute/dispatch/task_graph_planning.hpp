#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "compute/dirty/dirty_region_snapshot.hpp"
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
 * and ExecutionTaskRuntime.
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
 * @brief Callback-free operation route frozen by task-shape planning.
 *
 * @throws std::bad_alloc when copied exclusive-key storage cannot allocate.
 * @note The scalar identity and metadata originate from one coherent
 * OpRegistry snapshot. Planning deliberately retains no callback or plugin DSO
 * lease; execution must re-resolve the same nonzero identity before admission.
 */
struct PlannedOperationRoute {
  /** @brief Nonzero registry ownership revision of the selected callback. */
  std::uint64_t implementation_identity = 0U;
  /** @brief Device selected by registry intent/cost policy. */
  Device device = Device::CPU;
  /** @brief Complete scheduling and resource metadata from the same snapshot.
   */
  OpMetadata metadata;
  /** @brief Whether the selected callback uses the tiled operation shape. */
  bool tiled = false;
};

/**
 * @brief Copies one coherent registry implementation into a callback-free
 * route.
 *
 * @param implementation Selected registry value whose scalar identity,
 * device, metadata, and callback shape are frozen together.
 * @return Callback-free planning route.
 * @throws std::bad_alloc when copied metadata string storage cannot allocate.
 * @note The returned value never retains `implementation.func` or its plugin
 * DSO lease. Callers must keep the source implementation alive only for this
 * call.
 */
PlannedOperationRoute make_planned_operation_route(
    const OpImplementation& implementation);

/**
 * @brief Compares every execution-relevant field of two planned routes.
 *
 * @param lhs First callback-free route.
 * @param rhs Second callback-free route.
 * @return True only when identity, device, callback shape, and every
 * `OpMetadata` field match.
 * @throws Nothing.
 * @note Keep this centralized comparison and retained-memory accounting in
 * sync whenever `OpMetadata` gains a field.
 */
bool planned_operation_routes_equal(const PlannedOperationRoute& lhs,
                                    const PlannedOperationRoute& rhs) noexcept;

/**
 * @brief Checks one current registry implementation against a frozen route.
 *
 * @param route Callback-free planning authority.
 * @param implementation Newly selected callable snapshot.
 * @return True only when the current implementation is exactly the same
 * revision, device, shape, and metadata.
 * @throws Nothing.
 * @note The comparison never invokes or copies the callback and therefore
 * cannot extend a plugin DSO lifetime.
 */
bool planned_operation_route_matches(
    const PlannedOperationRoute& route,
    const OpImplementation& implementation) noexcept;

/**
 * @brief Readiness state accepted while checking one planned NodeOutput.
 *
 * @throws Nothing for ordinary enum operations.
 * @note Execution staging may retain the exact Pending Value so a supervised
 * continuation can observe it. Formal graph publication requires Ready.
 */
enum class PlannedOutputReadiness {
  /** @brief Accept Ready or Pending, but reject terminal failure states. */
  AllowPending,
  /** @brief Accept only a successfully Ready publication. */
  RequireReady,
};

/**
 * @brief Callback-free exact output authority frozen with a planned route.
 *
 * @throws std::bad_alloc when copied output-name storage cannot allocate.
 * @note The authority originates only from revisioned registry metadata and
 * graph extent inference. Provider-returned names, descriptors, layouts, and
 * identities never create or widen this plan.
 */
struct PlannedOutputAuthority {
  /** @brief Registry revision whose declared output schema was frozen. */
  std::uint64_t implementation_identity = 0U;
  /** @brief Device route associated with the declaration for diagnostics. */
  Device route_device = Device::CPU;
  /** @brief Required canonical image name, or absent for non-image routes. */
  std::optional<std::string> image_output_name;
  /** @brief Sorted exact required generic Value names excluding `image`. */
  std::vector<std::string> named_value_output_names;
  /** @brief Sorted exact required non-image parameter-result names. */
  std::vector<std::string> parameter_output_names;
  /** @brief Required representation for the canonical image Value. */
  ValueRepresentationKind image_representation =
      ValueRepresentationKind::DenseTensor;
  /** @brief Required layout family for the canonical image Value. */
  StorageLayoutKind image_layout = StorageLayoutKind::Strided;
  /** @brief Whether the canonical image requires an ordinary ImageFacet. */
  bool image_facet_required = true;
  /** @brief Positive planned image extent, or absent when legitimately dynamic.
   */
  std::optional<PixelSize> image_extent;
};

/**
 * @brief Derives one immutable output authority from trusted planning facts.
 *
 * @param route Coherent callback-free registry route.
 * @param resolved_extent Graph-inferred output extent; nonpositive dimensions
 * leave the image extent dynamic while preserving structural requirements.
 * @return Exact required image/generic/parameter schema and structural image
 * contract.
 * @throws GraphError with ComputeError for an invalid route identity or an
 * inconsistent partially positive extent.
 * @throws std::bad_alloc when copied name storage cannot allocate.
 * @note No provider callback or provider-produced output participates.
 */
PlannedOutputAuthority make_planned_output_authority(
    const PlannedOperationRoute& route, const PixelSize& resolved_extent);

/**
 * @brief Validates one result against a previously frozen output authority.
 *
 * @param output Provider or Host-produced request-local result.
 * @param authority Trusted exact output declaration from planning.
 * @param readiness Whether Pending may remain staged or Ready is mandatory.
 * @return Nothing after exact category names, generic representation/layout,
 * image descriptor/facet/shape/layout, identity, and readiness validation
 * succeeds.
 * @throws GraphError with ComputeError for compatibility staging, missing,
 * extra, invalid, structurally mismatched, or disallowed-readiness output.
 * @throws std::logic_error or std::overflow_error only if a supposedly valid
 * Value violates its own immutable metadata invariants.
 * @note The check maps no payload, waits on no fence, invokes no provider, and
 * mutates neither graph nor output state.
 */
void validate_planned_output(const NodeOutput& output,
                             const PlannedOutputAuthority& authority,
                             PlannedOutputReadiness readiness);

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
  PixelRect from_roi;
  /** @brief Downstream ROI demanded or affected by this edge, when known. */
  PixelRect to_roi;
  /** @brief Direction used to explain how dirty ROI mapping was derived. */
  DirtyEdgeDirection direction = DirtyEdgeDirection::BackwardDemand;
};

/**
 * @brief Executable task entry produced by graph planning.
 *
 * PlannedTask is the immutable planning record that later becomes an execution
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
  PixelRect output_roi;
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
  PixelRect represented_hp_roi;
  /** @brief Domain-local ROI the planner expects to execute. */
  PixelRect execution_roi;
  /** @brief Whether the work item must recompute the whole output. */
  bool whole_output = false;
  /**
   * @brief Whether planning observed exact complete formal HP cache.
   *
   * @note This is request-scoped diagnostic metadata, not a durable cache claim
   * or dirty execution decision. Snapshot selection never promotes it into
   * satisfaction.
   */
  bool reusable_cache_available = false;
  /** @brief Dirty ROIs associated with this node after snapshot pruning. */
  std::vector<PixelRect> dirty_rois;
  /** @brief Upstream planned node ids required by this node. */
  std::vector<int> dependency_node_ids;
  /** @brief Downstream planned node ids depending on this node. */
  std::vector<int> dependent_node_ids;
  /** @brief Task ids in ComputeTaskGraph::tasks that belong to this node. */
  std::vector<int> task_ids;
  /**
   * @brief Callback-free route selected while this node's task shape was built.
   * @note Graphless compatibility plans may leave this empty. Product graph
   * execution re-resolves and validates the exact identity before admission.
   */
  std::optional<PlannedOperationRoute> operation_route;
  /**
   * @brief Exact callback-free output contract frozen with operation_route.
   * @note Graphless compatibility plans may leave this empty. Every product
   * graph execution path must fail closed before staging or publication when
   * the authority is absent.
   */
  std::optional<PlannedOutputAuthority> output_authority;
};

/**
 * @brief Task-level graph for one planned compute domain.
 *
 * ComputeTaskGraph stores executable tasks, node-level dependency metadata,
 * and initial task ids derived from dependency_task_ids. It is immutable after
 * execution closures are built for a dispatch.
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
 * retained complete request-cone plan remains immutable across repeated ROI
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
  PixelRect represented_hp_roi;
  /** @brief Domain-local ROI selected for execution. */
  PixelRect execution_roi;
  /** @brief Whether selected dirty work covers the whole output. */
  bool whole_output = false;
  /** @brief Dirty ROIs associated with this node in the snapshot. */
  std::vector<PixelRect> dirty_rois;
};

/**
 * @brief Generation-local active task view over an immutable ComputePlan.
 *
 * The overlay records which already-expanded PlannedTask ids are active for a
 * dirty snapshot, their task-level dependency ids after snapshot ROI mappings,
 * and the source/downstream work sets used by source-first dirty execution.
 * It avoids copying the full ComputePlan on high-frequency dirty paths.
 *
 * @note Task ids refer to the retained request-cone ComputePlan used to create
 * the overlay. The overlay never creates new task shapes.
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
 * parallel execution preference, optional dirty ROI, and whether formal HP
 * cache may satisfy requested work. It is narrower than internal
 * ComputeService request options and contains only data needed to produce a
 * ComputePlan.
 *
 * @note RealTimeUpdate requests are still planned as a single domain per call;
 * HP/RT sibling coordination happens outside this struct. Force-recache
 * callers disable reusable cache before pruning instead of clearing visible
 * Graph output during fallible preparation.
 */
struct ComputeRequest {
  /** @brief Compute intent whose domain controls task expansion. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  /** @brief Target graph node id requested by the caller. */
  int target_node_id = -1;
  /** @brief Whether the caller intends route-backed execution. */
  bool parallel = false;
  /** @brief Optional dirty ROI used by dirty update callers. */
  std::optional<PixelRect> dirty_roi;
  /**
   * @brief Whether exact complete formal HP cache may satisfy planned work.
   *
   * @note This flag never promotes RT proxy or partial Region state. Ordinary
   * pruning delegates exact completeness to ComputeCachePolicy. Dirty
   * selection does not apply it to snapshot-selected work.
   */
  bool allow_reusable_cache = true;
  /**
   * @brief Whether dirty selection owns final request-cone demand cutting.
   *
   * @note Dirty preparation enables this after Region planning so node/cache
   * pruning retains the complete callback-free request cone. Ordinary full HP
   * dispatch leaves it disabled and may consume exact cache immediately.
   */
  bool defer_reusable_cache_pruning = false;
};

/**
 * @brief Cache-pruned or dirty-pruned plan for one compute request.
 *
 * ComputePlan records the target request, node execution order, per-node work,
 * and task graph used by sequential, parallel, and dirty update execution. It
 * is the stable topology contract for one request while execution runtime
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
  /** @brief Whether the caller intended route-backed execution. */
  bool parallel = false;
  /**
   * @brief Traversal order retained for this request plan.
   *
   * @note Ordinary plans contain the cache-pruned executable order. Dirty plans
   * retain the complete request cone until snapshot selection.
   */
  std::vector<int> execution_order;
  /**
   * @brief Node ids whose callback-free task shapes remain in this plan.
   *
   * @note Dirty plans include inactive boundary nodes and exclusive upstream
   * shape so selection can apply request-local demand cuts without destroying
   * task identities.
   */
  std::vector<int> planned_nodes;
  /**
   * @brief Request-cone work and cache-boundary summaries.
   *
   * Records for retained task-shape nodes carry task ids and appear in
   * planned_nodes. Ordinary cache-pruned plans may also retain metadata-only
   * records for boundaries and exclusive upstream work. Dirty plans retain task
   * ids for the complete request cone until snapshot demand cutting.
   */
  std::vector<PlannedNodeWork> planned_work;
  /** @brief Executable task graph derived from planned work. */
  ComputeTaskGraph task_graph;
  /** @brief Canonical device inventory used for operation route selection. */
  std::vector<Device> available_devices;
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
 * pointer is optional and must not be used by workers for runtime state.
 */
struct ComputePlanSummary {
  /** @brief Compute intent represented by the summarized plan. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  /** @brief Target node id from the request. */
  int target_node_id = -1;
  /** @brief Whether the caller intended route-backed execution. */
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
  /** @brief Canonical device inventory covered by this cached expansion. */
  std::vector<Device> available_devices;
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
   * @param available_devices Canonical route-visible device inventory.
   * @return FullTaskGraph containing all expanded node work and tasks.
   * @throws GraphError or standard exceptions from graph access, extent
   * resolution, op metadata lookup, or allocation.
   */
  FullTaskGraph expand(const GraphModel& graph, ComputeIntent intent,
                       const std::vector<Device>& available_devices = {
                           Device::CPU}) const;
};

/**
 * @brief Prunes a FullTaskGraph to a target/cache-aware request plan.
 *
 * @note Exact complete formal HP cache forms a request-local read boundary for
 * ordinary requests. Dirty requests retain the complete callback-free request
 * cone and only record the planning-time observation; a node selected by the
 * dirty snapshot remains executable because its old bytes are a merge base,
 * not proof that the requested Region is current.
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
   * @return Ordinary ComputePlan limited to executable demand before cache
   * boundaries, or a complete request-cone plan when deferred dirty selection
   * is enabled.
   * @throws GraphError when requested nodes are missing from graph or full
   * expansion.
   * @throws std::logic_error, std::invalid_argument, std::overflow_error, or
   * std::bad_alloc when exact cache validity or result storage cannot be
   * evaluated.
   * @note RealTimeUpdate and requests with allow_reusable_cache=false never
   * treat formal HP cache as executable-task satisfaction. Deferred dirty
   * plans preserve tasks and dependencies even when planning observes cache.
   */
  ComputePlan prune(const FullTaskGraph& full_graph,
                    const ComputeRequest& request,
                    const std::vector<int>& execution_order,
                    const GraphModel& graph) const;
};

/**
 * @brief Applies a DirtyRegionSnapshot to a request-cone plan.
 *
 * DirtySnapshotTaskGraphPruner annotates already-expanded tasks with dirty
 * metadata and materializes source/downstream task id groups. It does not
 * create new task shapes.
 *
 * @note The input plan must already be single-domain. Dirty preparation retains
 * the complete request cone and delegates snapshot/external boundary demand
 * cutting to select().
 */
class DirtySnapshotTaskGraphPruner {
 public:
  /**
   * @brief Selects dirty tasks without copying the request-cone plan.
   *
   * @param node_cache_plan Immutable plan produced by
   * NodeCacheTaskGraphPruner with dirty cache pruning deferred.
   * @param snapshot Graph-scoped dirty facts for the same compute domain.
   * @param graph Graph used for exact task-level ROI dependencies.
   * @param externally_satisfied_node_ids Optional request-local node identities
   * whose outputs are already staged and must not execute in this phase.
   * @return Generation-local active task overlay and source/downstream groups.
   * @throws std::logic_error, std::invalid_argument, std::overflow_error, or
   * std::bad_alloc when dependency metadata or overlay storage cannot be
   * evaluated.
   * @note This is the dirty execution path: it does not mutate or duplicate
   * PlannedTask records, and it preserves task ids from node_cache_plan.
   * Selection never lets old formal cache satisfy dirty work. Explicit
   * current-request external satisfaction may suppress a dirty candidate.
   * Demand traversal includes inactive connector and externally satisfied
   * boundary nodes, stops at each boundary, and emits only dirty candidates.
   * Dependencies on explicitly satisfied nodes remain outside the active view
   * and are therefore treated as completed without reusing their task ids.
   * Upstream tasks needed only through a satisfied boundary are also removed;
   * shared upstream work remains active when another unsatisfied sink needs it.
   */
  DirtyTaskSelectionOverlay select(
      const ComputePlan& node_cache_plan, const DirtyRegionSnapshot& snapshot,
      const GraphModel& graph,
      const std::unordered_set<int>* externally_satisfied_node_ids =
          nullptr) const;

  /**
   * @brief Annotates and clips a request-scoped plan with dirty metadata.
   *
   * @param node_cache_plan Request-scoped plan produced by
   * NodeCacheTaskGraphPruner. Dirty execution supplies the retained complete
   * request cone.
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
 * @param available_devices Route-visible device inventory.
 * @return Cache key covering topology generation, intent, task-shape
 * configuration version, and operation-registry task-shape generation.
 * @throws std::bad_alloc if string construction fails.
 * @note The shape config token must change when tile sizing or task shape
 *       selection semantics change. A plugin callback-shape override or unload
 *       advances the registry generation and cannot reuse predecessor tasks.
 */
std::string full_task_graph_cache_key(
    const GraphModel& graph, ComputeIntent intent,
    const std::vector<Device>& available_devices = {Device::CPU});

/**
 * @brief Returns a cached immutable FullTaskGraph or expands and stores one.
 *
 * @param graph GraphModel owning the per-topology full graph cache.
 * @param intent Compute intent whose single-domain full graph is required.
 * @param available_devices Route-visible device inventory.
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
    GraphModel& graph, ComputeIntent intent,
    const std::vector<Device>& available_devices = {Device::CPU});

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
