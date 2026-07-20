#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "compute/compute_task_dependency_state.hpp"
#include "compute/compute_task_dispatcher.hpp"
#include "compute/dirty_region_planner.hpp"
#include "compute/execution_service.hpp"
#include "compute/resource_demand_estimator.hpp"
#include "compute/task_graph_planning.hpp"
#include "core/ps_types.hpp"      // NOLINT(build/include_subdir)
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "photospider/scheduler/scheduler.hpp"

namespace ps {
class GraphModel;
class GraphRuntime;
class SchedulerHostContext;
}  // namespace ps

namespace ps::compute {

/**
 * @brief Owns per-node critical sections for one dirty update transaction.
 *
 * Dirty scheduler callbacks use these mutexes while copying a live `Node`,
 * resolving its format-neutral runtime parameters, and touching request-local
 * staging metadata. A scheduler-backed `RealTimeUpdate` shares one instance
 * between its concurrent HP and RT siblings so both domains synchronize access
 * to the same live node without coupling their task graphs or output buffers.
 *
 * @throws std::bad_alloc If node-id or mutex storage cannot be allocated.
 * @throws GraphError If checked retained-memory estimation overflows.
 * @note The node map is fully constructed before scheduler submission and is
 * immutable thereafter. Different node ids remain independent, and selected
 * operation execution occurs outside these critical sections. Ownership is
 * request-scoped; no instance is stored in GraphModel, GraphRuntime, or global
 * state. The retained-memory contract includes the shared allocation,
 * immutable map, and every independently owned mutex while excluding
 * allocator-private metadata.
 */
class DirtyNodeSynchronization final {
 public:
  /**
   * @brief Creates one mutex for every node visible to the dirty transaction.
   * @param node_ids Stable graph node identities; duplicate values are ignored.
   * @throws std::bad_alloc If the mutex map or one mutex cannot be allocated.
   * @note Construction must finish before HP/RT sibling callbacks start. The
   * caller must keep this object alive until every scheduler callback drains.
   */
  explicit DirtyNodeSynchronization(const std::vector<int>& node_ids);

  /**
   * @brief Returns the immutable transaction's mutex for one graph node.
   * @param node_id Node whose live snapshot or staging state will be touched.
   * @return Mutable mutex owned by this synchronization object.
   * @throws std::out_of_range If node_id was not present at construction,
   * indicating a topology/task materialization mismatch.
   * @note The returned reference remains valid for this object's lifetime; the
   * map is never mutated after construction.
   */
  std::mutex& mutex_for(int node_id) const;

  /**
   * @brief Estimates complete Host-owned synchronization storage.
   * @return Checked object, shared-control, unordered-map bucket/value/linkage,
   * unique-owner, and independently allocated mutex bytes.
   * @throws GraphError when checked structural arithmetic overflows.
   * @note The estimate includes one shared control block because callers retain
   * this object through `std::shared_ptr`. The unique_ptr owner itself is
   * already part of each unordered-map value; every pointee adds one
   * `sizeof(std::mutex)`. Allocator-private node metadata and any opaque
   * platform mutex allocation beyond the visible C++ object are excluded.
   * The immutable map permits concurrent calls without additional locking.
   */
  std::uint64_t retained_memory_bytes() const;

 private:
  /** @brief Immutable node-id index of independently owned mutexes. */
  std::unordered_map<int, std::unique_ptr<std::mutex>> node_mutexes_;
};

/**
 * @brief Returns the running legacy scheduler registered for one intent.
 *
 * This helper preserves the transitional scheduler route used by full graph
 * dispatch and dirty ROI dispatch. It looks up the complete legacy
 * `IScheduler` registered on the supplied GraphRuntime and starts it when
 * necessary. Ownerless built-in CPU routes use ExecutionService instead.
 *
 * @param runtime Per-graph runtime that owns the requested legacy scheduler.
 * @param intent Compute intent whose scheduler should receive work.
 * @return Running scheduler for the requested intent.
 * @throws GraphError when no scheduler is registered.
 * @throws Any scheduler start exception unchanged.
 * @note The returned reference remains owned by GraphRuntime. Callers must not
 * invoke this helper for a process-service binding or store the reference
 * beyond the active compute request.
 */
IScheduler& ensure_running_scheduler(GraphRuntime& runtime,
                                     ComputeIntent intent);

/**
 * @brief Bounded dirty planning result used by HP and RT executors.
 *
 * The prepared state packages the graph-scoped dirty snapshot, the
 * immutable node/cache-pruned compute plan, the generation-local dirty
 * selection overlay, and the materialized source/downstream task groups that
 * will be submitted to the selected physical execution domain. The dirty plan
 * itself owns the per-node HP or RT ROI entries used by node execution.
 *
 * @tparam DirtyPlan HighPrecisionDirtyPlan or RealTimeDirtyPlan.
 * @note The struct is request-local. It must not be stored after execution
 * callbacks derived from it have drained.
 */
template <typename DirtyPlan>
struct PreparedDirtyPlan {
  /** @brief Dirty planner output with per-node execution entries. */
  DirtyPlan dirty_plan;

  /** @brief Node/cache-pruned compute plan used as immutable task shape. */
  ComputePlan compute_plan;

  /** @brief Dirty active task view over compute_plan for this generation. */
  DirtyTaskSelectionOverlay selection;

  /** @brief Task id groups selected by DirtySnapshotTaskGraphPruner. */
  DirtyUpdateWorkSet work_set;

  /** @brief Dirty source task ids selected by materialization. */
  std::vector<int> source_task_ids;

  /** @brief Downstream dirty task ids selected by materialization. */
  std::vector<int> downstream_task_ids;
};

/**
 * @brief Immutable parameters for source-first dirty task dispatch.
 *
 * The request object lowers helper parameter count and makes the dispatch
 * contract explicit: source boundary work completes before downstream work.
 * Legacy scheduler batches allocate their own epochs; process-service batches
 * use Run identity. Dirty generation remains separate snapshot and
 * source-commit provenance.
 */
struct DirtySourceFirstRunRequest {
  /** @brief Optional runtime owning the active legacy scheduler and traces. */
  GraphRuntime* runtime = nullptr;

  /** @brief Intent whose physical execution domain receives the work. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;

  /**
   * @brief Injected process CPU service, or null for inline/legacy routing.
   */
  ExecutionService* execution_service = nullptr;

  /**
   * @brief Observation target borrowed for process-service settlement.
   *
   * @note Required exactly when execution_service is non-null and valid until
   * every synchronous service batch in this request has drained.
   */
  SchedulerHostContext* host = nullptr;

  /**
   * @brief Single-domain Run owning service submission leases.
   *
   * @note Required exactly when execution_service is non-null.
   */
  ComputeRun* run = nullptr;

  /** @brief Compute plan containing immutable dirty task graph metadata. */
  const ComputePlan* compute_plan = nullptr;

  /** @brief Dirty generation-local active task view for dependency release. */
  const DirtyTaskSelectionOverlay* selection = nullptr;

  /**
   * @brief Other request-owned structural bytes retained across phase service
   * settlement.
   *
   * @note This excludes the copied `DirtyReadyTaskContext` and matching Run
   * control, which the adapter adds itself. Image pixels and opaque
   * backend/plugin/device allocations must not be declared here without a
   * trusted size contract.
   */
  std::uint64_t additional_shared_retained_memory_bytes = 0U;

  /**
   * @brief Computes phase-local retained bytes immediately before admission.
   *
   * @note The callback receives the exact source or downstream task ids for
   * that synchronous segment. It may inspect request-owned staging state to
   * charge current storage plus predictable missing map entries without
   * recounting keys created by an earlier source phase. It must not execute
   * operations or mutate staging state.
   */
  std::function<std::uint64_t(const std::vector<int>&)>  // NOLINT
      phase_shared_retained_memory_bytes;

  /** @brief Dirty source task ids submitted before downstream work. */
  const std::vector<int>* source_task_ids = nullptr;

  /** @brief Downstream dirty task ids released by task dependencies. */
  const std::vector<int>* downstream_task_ids = nullptr;

  /**
   * @brief Dirty snapshot generation associated with this request.
   * @note The current source-first runner does not forward this value as the
   *       scheduler epoch or service Run id; legacy scheduler initial batches
   *       allocate independent scheduler-local epochs.
   */
  uint64_t dirty_generation = 0;

  /** @brief Boundary validation invoked between source and downstream groups.
   */
  std::function<void()> before_downstream;
};

/**
 * @brief Heap-owned dirty phase context for process-service submissions.
 *
 * The context owns a copy of the immutable compute plan, optional dirty
 * selection, active task membership, dependency counters, task callable, and
 * one matching Run lease. Each materialized `ReadyTaskSubmission` retains this
 * context and another matching lease, so no stack `TaskExecutor*` crosses the
 * process-service boundary.
 *
 * @throws std::bad_alloc if plan, selection, dependency, callable, or task
 * ownership cannot allocate.
 * @note Stable Graph/cache/staging references captured by the owned callable
 * remain protected by Graph-state admission and synchronous Run settlement.
 */
class DirtyReadyTaskContext final
    : public std::enable_shared_from_this<DirtyReadyTaskContext> {
 public:
  /**
   * @brief Builds one owned source or downstream dirty phase.
   *
   * @param compute_plan Immutable task graph copied into context ownership.
   * @param selection Optional dirty dependency overlay copied into ownership.
   * @param active_task_ids Exact task ids active in this phase.
   * @param run_task Owned callable that executes one dense task id.
   * @param run_task_retained_memory_bytes Audited capture/allocation bytes
   * owned by run_task beyond its inline `std::function` object. This value
   * covers exactly the callable target moved into this context; a
   * simultaneously live external copy must be declared separately by the
   * owning phase.
   * @param lease Matching Run lease retained for dependent submissions.
   * @param release_dependents Whether completion releases dependency-ready
   * work from this phase.
   * @param priority Process queue hint for every phase submission.
   * @throws std::invalid_argument if run_task is empty.
   * @throws std::bad_alloc from owned state construction.
   */
  DirtyReadyTaskContext(const ComputePlan& compute_plan,
                        const DirtyTaskSelectionOverlay* selection,
                        const std::vector<int>& active_task_ids,
                        std::function<void(int)> run_task,
                        std::uint64_t run_task_retained_memory_bytes,
                        ComputeRunLease lease, bool release_dependents,
                        SchedulerTaskPriority priority);

  /**
   * @brief Estimates complete context-owned structural storage.
   * @return Checked copied plan/selection/dependency/callable/context bytes.
   * @throws GraphError when checked structural arithmetic overflows.
   * @note The shared Run control is intentionally excluded and added once by
   * `run_resource_demand()`. Borrowed Graph/cache/staging references and opaque
   * output payloads are excluded.
   */
  std::uint64_t retained_memory_bytes() const;

  /**
   * @brief Builds the complete adapter declaration for this dirty phase.
   * @param additional_shared_retained_memory_bytes Other request-owned
   * structural bytes that remain live across phase settlement.
   * @return Shared context/Run bytes plus uniform shared-pointer capture
   * demand.
   * @throws GraphError when checked structural arithmetic overflows.
   * @throws std::system_error when matching Run storage locking fails.
   */
  CpuRunResourceDemand run_resource_demand(
      std::uint64_t additional_shared_retained_memory_bytes) const;

  /**
   * @brief Materializes owned submissions for selected ready task ids.
   *
   * @param task_ids Dependency-ready ids within this phase.
   * @param initial_ready Whether these values form the initial phase batch.
   * @return Move-owned submissions retaining this context and matching leases.
   * @throws std::invalid_argument for an inactive or invalid task id.
   * @throws std::bad_alloc when output, executable, or lease storage allocates.
   * @note Readiness is caller-established; this method performs membership and
   * identity validation only.
   */
  std::vector<ReadyTaskSubmission> make_submissions(
      const std::vector<int>& task_ids, bool initial_ready);

 private:
  /**
   * @brief Executes one matching service callback and releases dependents.
   *
   * @param lease Submission-owned lease naming this context's Run.
   * @param identity Composite identity whose local id selects the dirty task.
   * @param task_runtime Active service runtime used for ready release,
   * completion, and failure trace.
   * @return Nothing.
   * @throws std::invalid_argument for mismatched or inactive identity.
   * @throws Exact task, dependency, submission, trace, or completion
   * exception.
   */
  void execute(ComputeRunLease& lease, const ComputeRunTaskIdentity& identity,
               SchedulerTaskRuntime& task_runtime);

  /** @brief Immutable task shape copied into Run-phase ownership. */
  ComputePlan compute_plan_;

  /** @brief Optional copied dirty dependency and readiness overlay. */
  std::optional<DirtyTaskSelectionOverlay> selection_;

  /** @brief Exact active ids retained for dependency-state construction. */
  std::vector<int> active_task_ids_;

  /** @brief Fast active membership guard for composite identity validation. */
  std::unordered_set<int> active_task_id_set_;

  /** @brief Owned dependency counters and dependent adjacency. */
  std::unique_ptr<TaskDependencyState> dependency_state_;

  /** @brief Owned dirty node/task callable. */
  std::function<void(int)> run_task_;

  /** @brief Dynamic capture/allocation bytes owned by `run_task_`. */
  std::uint64_t run_task_retained_memory_bytes_ = 0U;

  /** @brief Base matching lease copied into every materialized submission. */
  ComputeRunLease lease_;

  /** @brief Whether task completion releases ready dependents. */
  bool release_dependents_ = false;

  /** @brief Process ready-queue hint for this phase. */
  SchedulerTaskPriority priority_ = SchedulerTaskPriority::Normal;
};

/**
 * @brief Stores the latest dirty snapshot and bounded history on the graph.
 *
 * @param graph Graph whose inspection state receives the snapshot.
 * @param snapshot Dirty-region snapshot generated for the current request.
 * @throws std::bad_alloc if snapshot history storage cannot grow.
 * @note The history cap mirrors ComputeService's existing inspection policy.
 */
void remember_dirty_snapshot(GraphModel& graph,
                             const DirtyRegionSnapshot& snapshot);

/**
 * @brief Stores the latest compute plan and bounded summary history.
 *
 * @param graph Graph whose inspection state receives the compute plan.
 * @param compute_plan Node/cache-pruned plan for the current request.
 * @param selection Optional dirty overlay used to summarize active work.
 * @throws std::bad_alloc if summary history storage cannot grow.
 * @note Full plans are retained only as the latest inspection entry; repeated
 * history stores summaries to avoid copying large task graphs.
 */
void remember_compute_plan(
    GraphModel& graph, const ComputePlan& compute_plan,
    const DirtyTaskSelectionOverlay* selection = nullptr);

/**
 * @brief Prunes a full task graph to the current request and cache state.
 *
 * @param graph Graph that supplies topology, node metadata, and cache state.
 * @param request Intent, target node, and dirty ROI for the request.
 * @param execution_order Topological order selected by dirty planning.
 * @return Node/cache-pruned compute plan before dirty snapshot selection.
 * @throws GraphError from task graph expansion or pruning.
 * @note The returned plan is still domain-specific and contains no mixed HP/RT
 * task pool.
 */
ComputePlan prune_node_cache_task_graph(
    GraphModel& graph, const ComputeRequest& request,
    const std::vector<int>& execution_order);

/**
 * @brief Applies dirty snapshot pruning to a node/cache-pruned plan.
 *
 * @param node_cache_plan Plan already scoped to target and cache state.
 * @param snapshot Dirty snapshot for the same compute domain.
 * @param graph Graph used to derive per-tile input ROI dependencies.
 * @return Dirty-pruned plan with selected tasks annotated.
 * @throws GraphError from dirty snapshot pruning.
 * @note This helper does not create new tasks; it only selects or clips tasks
 * already expanded in the request plan.
 */
ComputePlan prune_dirty_snapshot_task_graph(const ComputePlan& node_cache_plan,
                                            const DirtyRegionSnapshot& snapshot,
                                            const GraphModel& graph);

/**
 * @brief Resolves phase task ids to unique node ids for retained admission.
 *
 * @param compute_plan Plan containing task-to-node ownership.
 * @param task_ids Exact task ids selected for one service segment.
 * @return Node ids in first-task occurrence order with duplicates removed.
 * @throws std::out_of_range when a task id is outside the plan.
 * @throws std::bad_alloc if temporary set or vector allocation fails.
 * @note The result predicts Host-owned per-node staging entries only. It
 * carries no scheduling, dependency, or execution authority.
 */
std::vector<int> planned_nodes_for_task_ids(const ComputePlan& compute_plan,
                                            const std::vector<int>& task_ids);

/**
 * @brief Verifies dirty source boundary outputs before downstream work starts.
 *
 * @param graph Graph whose dirty source nodes are inspected.
 * @param snapshot Dirty snapshot containing source node ids.
 * @param domain HP or RT dirty domain used to select cache authority.
 * @throws GraphError when a dirty source node is missing or its boundary
 * output is unavailable.
 * @note The check runs after source tasks and before downstream tasks to keep
 * dependency failures deterministic across inline and scheduler execution.
 */
void validate_dirty_source_boundaries_ready(const GraphModel& graph,
                                            const DirtyRegionSnapshot& snapshot,
                                            DirtyDomain domain);

/**
 * @brief Checks whether a node is listed as a dirty source in a snapshot.
 *
 * @param snapshot Dirty snapshot for the current request.
 * @param node_id Node id being inspected.
 * @return True when the node is a dirty source boundary.
 * @throws Nothing directly.
 * @note Source membership controls stale-generation checks and trace labels.
 */
bool is_dirty_source_node(const DirtyRegionSnapshot& snapshot, int node_id);

/**
 * @brief Logs generic and dirty-role execution events for one node.
 *
 * @param runtime Optional runtime that owns the scheduler trace log.
 * @param node_id Node being executed.
 * @param dirty_source Whether the node is a source boundary or downstream
 * dirty work.
 * @throws Any exception propagated by GraphRuntime::log_event.
 * @note A null runtime preserves inline execution behavior by emitting no
 * scheduler trace entries.
 */
void log_dirty_node_execution(GraphRuntime* runtime, int node_id,
                              bool dirty_source);

/**
 * @brief Logs and skips stale dirty source generations.
 *
 * @param runtime Optional runtime for scheduler trace events.
 * @param node_id Dirty source node id.
 * @param committed_generation Generation already committed for this source.
 * @param dirty_generation Generation being executed by the current request.
 * @return True when the current request should skip the node.
 * @throws Any exception propagated by GraphRuntime::log_event.
 * @note The comparison intentionally preserves the strict-greater policy so
 * repeated execution of the same generation is still allowed.
 */
bool should_skip_stale_dirty_source(GraphRuntime* runtime, int node_id,
                                    uint64_t committed_generation,
                                    uint64_t dirty_generation);

/**
 * @brief Infers image channels and data type for a reused or new output buffer.
 *
 * @param preferred Existing output preferred for the target intent.
 * @param image_inputs Ready image inputs for the node.
 * @param fallback Optional secondary output used as a final shape hint.
 * @return Pair of channel count and data type.
 * @throws Nothing directly.
 * @note Defaults to one FLOAT32 channel when neither output nor input carries
 * concrete image metadata, matching the pre-split dirty update behavior.
 */
std::pair<int, DataType> infer_output_spec(
    const std::optional<NodeOutput>& preferred,
    const std::vector<const NodeOutput*>& image_inputs,
    const std::optional<NodeOutput>* fallback = nullptr);

/**
 * @brief Applies dirty-pruned HP ROI overrides back to HP plan entries.
 *
 * @param entries Per-node HP execution entries from DirtyRegionPlanner.
 * @param selection Dirty active task overlay selected for execution.
 * @throws Nothing directly.
 * @note The overlay may further clip represented HP ROIs; the executor must
 * use those clipped regions for node execution.
 */
void apply_planned_work_rois(std::unordered_map<int, HpPlanEntry>& entries,
                             const DirtyTaskSelectionOverlay& selection);

/**
 * @brief Applies dirty-pruned HP and RT ROI overrides back to RT plan entries.
 *
 * @param entries Per-node RT execution entries from DirtyRegionPlanner.
 * @param selection Dirty active task overlay selected for execution.
 * @throws Nothing directly.
 * @note HP-space ROI is used for inspection/version metadata, while execution
 * ROI is used to clip RT proxy buffer writes.
 */
void apply_planned_work_rois(std::unordered_map<int, RtPlanEntry>& entries,
                             const DirtyTaskSelectionOverlay& selection);

/**
 * @brief Prepares common dirty execution state after planner output exists.
 *
 * @tparam DirtyPlan HighPrecisionDirtyPlan or RealTimeDirtyPlan.
 * @param graph Request-local graph used for task shape, cache pruning, and
 * dirty selection. It may be a stabilized shadow graph.
 * @param dirty_plan Dirty planner output for one intent domain.
 * @param request Compute request matching the same dirty domain.
 * @param inspection_graph Optional authoritative live graph receiving plan and
 * snapshot diagnostics; null uses graph.
 * @param externally_satisfied_node_ids Optional nodes already executed by
 * parameter stabilization and excluded from phase-two task selection.
 * @return Prepared plan with node groups ready for task construction.
 * @throws GraphError from task graph pruning or materialization.
 * @note The helper updates authoritative inspection fields before execution so
 * failed execution still leaves planning evidence visible. Summary topology
 * identity and cache keys always come from inspection_graph, never from an
 * artificial shadow-graph generation.
 */
template <typename DirtyPlan>
PreparedDirtyPlan<DirtyPlan> prepare_dirty_execution(
    GraphModel& graph, DirtyPlan&& dirty_plan, const ComputeRequest& request,
    GraphModel* inspection_graph = nullptr,
    const std::unordered_set<int>* externally_satisfied_node_ids = nullptr) {
  GraphModel& inspection = inspection_graph ? *inspection_graph : graph;
  inspection.last_dirty_region_snapshot_debug =
      DirtyRegionPlanner::describe_snapshot(dirty_plan.snapshot);
  remember_dirty_snapshot(inspection, dirty_plan.snapshot);

  const ComputePlan node_cache_plan =
      prune_node_cache_task_graph(graph, request, dirty_plan.execution_order);
  DirtySnapshotTaskGraphPruner dirty_snapshot_pruner;
  DirtyTaskSelectionOverlay selection =
      dirty_snapshot_pruner.select(node_cache_plan, dirty_plan.snapshot, graph,
                                   externally_satisfied_node_ids);
  apply_planned_work_rois(dirty_plan.entries, selection);
  remember_compute_plan(inspection, node_cache_plan, &selection);

  DirtyUpdateWorkSet work_set = dirty_snapshot_pruner.materialize(selection);
  std::vector<int> source_task_ids = work_set.dirty_source_task_ids;
  std::vector<int> downstream_task_ids = work_set.downstream_task_ids;

  return PreparedDirtyPlan<DirtyPlan>{
      std::move(dirty_plan),      std::move(node_cache_plan),
      std::move(selection),       std::move(work_set),
      std::move(source_task_ids), std::move(downstream_task_ids)};
}

/**
 * @brief Task-id backed TaskExecutor used by dirty update task handles.
 *
 * The executor exposes compact TaskHandle entries and invokes one
 * request-local callable with the selected task id. For downstream dirty work
 * it also owns task-level TaskDependencyState so completed tasks release ready
 * dependents in batches.
 *
 * @note The executor is stack-owned by run_dirty_source_first() and must remain
 * alive until the matching SchedulerTaskRuntime::wait_for_completion() returns.
 * It does not allocate one std::function per active dirty task.
 */
template <typename RunTask>
class DirtyHandleTaskExecutor : public TaskExecutor {
 public:
  /**
   * @brief Binds task runner, dependency state, and scheduler runtime.
   *
   * @param compute_plan Plan whose task graph owns immutable task metadata.
   * @param selection Optional dirty overlay with dependency overrides.
   * @param active_task_ids Task ids active in this source or downstream phase.
   * @param run_task Callable invoked with one active dirty task id.
   * @param task_runtime Runtime used for dependent handle submission.
   * @param release_dependents Whether completed tasks should release
   * downstream dependents.
   * @param priority Priority used when dependency release submits ready work.
   * @throws std::bad_alloc if dependency state allocation fails.
   */
  DirtyHandleTaskExecutor(const ComputePlan& compute_plan,
                          const DirtyTaskSelectionOverlay* selection,
                          const std::vector<int>& active_task_ids,
                          RunTask& run_task, SchedulerTaskRuntime& task_runtime,
                          bool release_dependents,
                          SchedulerTaskPriority priority)
      : compute_plan_(compute_plan),
        dependency_state_(
            selection
                ? TaskDependencyState(compute_plan.execution_order,
                                      compute_plan.task_graph, active_task_ids,
                                      selection->dependency_task_ids)
                : TaskDependencyState(compute_plan.execution_order,
                                      compute_plan.task_graph,
                                      active_task_ids)),
        run_task_(run_task),
        task_runtime_(task_runtime),
        release_dependents_(release_dependents),
        priority_(priority) {
    task_handles_.resize(compute_plan_.task_graph.tasks.size());
    for (const auto& task : compute_plan_.task_graph.tasks) {
      if (task.task_id < 0 ||
          task.task_id >= static_cast<int>(task_handles_.size())) {
        continue;
      }
      task_handles_[task.task_id] =
          TaskHandle{this, task.task_id, task.node_id};
    }
  }

  /**
   * @brief Builds handles for selected task ids.
   *
   * @param task_ids Task ids to expose as scheduler handles.
   * @return Handles aligned with task_ids order, skipping invalid ids.
   * @throws std::bad_alloc if output allocation fails.
   */
  std::vector<TaskHandle> handles_for(const std::vector<int>& task_ids) const {
    std::vector<TaskHandle> handles;
    handles.reserve(task_ids.size());
    for (int task_id : task_ids) {
      if (task_id < 0 || task_id >= static_cast<int>(task_handles_.size())) {
        continue;
      }
      handles.push_back(task_handles_[task_id]);
    }
    return handles;
  }

  /**
   * @brief Executes one dirty task id and releases dependent task handles.
   *
   * @param task_id Dirty task id selected by scheduler.
   * @throws Any exception propagated by the dirty node executor.
   * @note Completion accounting mirrors TaskSubmissionPlan::run_task().
   */
  void run_task(int task_id) override {
    const auto& task = compute_plan_.task_graph.tasks.at(task_id);
    try {
      run_task_(task_id);
      if (release_dependents_) {
        std::vector<int> ready_ids =
            dependency_state_.release_dependents(task_id);
        task_runtime_.submit_ready_task_handles_from_worker(
            handles_for(ready_ids), priority_);
      }
    } catch (...) {
      task_runtime_.log_event(SchedulerTraceAction::RethrowException,
                              task.node_id);
      throw;
    }
    task_runtime_.dec_tasks_to_complete();
  }

 private:
  /** @brief Immutable node/cache-pruned compute plan whose tasks run. */
  const ComputePlan& compute_plan_;

  /** @brief Task-level dependency counters for this active phase. */
  TaskDependencyState dependency_state_;

  /** @brief Request-local dirty task runner called with a dense task id. */
  RunTask& run_task_;

  /** @brief Scheduler runtime borrowed for ready release and completion. */
  SchedulerTaskRuntime& task_runtime_;

  /** @brief Ready handles aligned with task id. */
  std::vector<TaskHandle> task_handles_;

  /** @brief Whether this executor releases downstream dependents. */
  bool release_dependents_ = false;

  /** @brief Priority used for ready dependent submissions. */
  SchedulerTaskPriority priority_ = SchedulerTaskPriority::Normal;
};

/**
 * @brief Runs dirty source tasks before downstream dirty tasks.
 *
 * @tparam RunTask Callable that executes one dirty task id.
 * @param request Source-first dispatch request and boundary validation.
 * @param run_task Task runner invoked with dense task ids.
 * @throws Exceptions from task construction, task execution, boundary
 * validation, scheduler lookup, or scheduler submission.
 * @note Legacy scheduler execution delegates source-first submission to
 * ComputeTaskDispatcher::submit_dirty_ready_tasks_source_first. Process
 * service execution materializes heap-owned Run submissions. Its source
 * context copies the outer `std::function`, so source admission charges both
 * live callable targets until synchronous settlement. Downstream transfers
 * that outer target by move and therefore charges only its context-owned
 * target. The dirty executor retains request-local inline fallback ordering.
 */
template <typename RunTask>
void run_dirty_source_first(const DirtySourceFirstRunRequest& request,
                            RunTask run_task) {
  const ComputePlan& compute_plan = *request.compute_plan;
  const std::vector<int>& source_task_ids = *request.source_task_ids;
  const std::vector<int>& downstream_task_ids = *request.downstream_task_ids;

  if (request.execution_service) {
    if (!request.host || !request.run) {
      throw std::invalid_argument(
          "Dirty process-service routing requires a host and Run.");
    }
    const std::uint64_t run_task_retained_memory_bytes =
        owned_callable_retained_memory_bytes(
            static_cast<std::uint64_t>(sizeof(RunTask)));
    std::function<void(int)> owned_run_task(std::move(run_task));
    ComputeRunLease phase_lease = request.run->acquire_lease();
    const auto additional_phase_retained_bytes =
        [&request](const std::vector<int>& task_ids) {
          RetainedMemoryEstimator estimate("dirty phase retained demand");
          estimate.add_bytes(request.additional_shared_retained_memory_bytes);
          if (request.phase_shared_retained_memory_bytes) {
            estimate.add_bytes(
                request.phase_shared_retained_memory_bytes(task_ids));
          }
          return estimate.bytes();
        };

    if (!source_task_ids.empty()) {
      auto source_context = std::make_shared<DirtyReadyTaskContext>(
          compute_plan, request.selection, source_task_ids, owned_run_task,
          run_task_retained_memory_bytes, phase_lease, false,
          SchedulerTaskPriority::High);
      std::vector<ReadyTaskSubmission> source_submissions =
          source_context->make_submissions(source_task_ids, true);
      RetainedMemoryEstimator source_phase_retained(
          "dirty source phase retained demand");
      source_phase_retained.add_bytes(
          additional_phase_retained_bytes(source_task_ids));
      source_phase_retained.add_bytes(run_task_retained_memory_bytes);
      request.execution_service->execute_cpu_run(
          *request.host, std::move(source_submissions),
          static_cast<int>(source_task_ids.size()),
          source_context->run_resource_demand(source_phase_retained.bytes()));
    }
    if (request.before_downstream) {
      request.before_downstream();
    }

    if (!downstream_task_ids.empty()) {
      std::vector<int> initial_downstream_ids;
      if (request.selection) {
        initial_downstream_ids = request.selection->initial_downstream_task_ids;
      } else {
        TaskGraphReadyChecker ready_checker;
        initial_downstream_ids = ready_checker.initial_ready_task_ids(
            compute_plan.task_graph, &downstream_task_ids);
      }
      auto downstream_context = std::make_shared<DirtyReadyTaskContext>(
          compute_plan, request.selection, downstream_task_ids,
          std::move(owned_run_task), run_task_retained_memory_bytes,
          phase_lease, true,
          request.intent == ComputeIntent::RealTimeUpdate
              ? SchedulerTaskPriority::High
              : SchedulerTaskPriority::Normal);
      std::vector<ReadyTaskSubmission> downstream_submissions =
          downstream_context->make_submissions(initial_downstream_ids, true);
      request.execution_service->execute_cpu_run(
          *request.host, std::move(downstream_submissions),
          static_cast<int>(downstream_task_ids.size()),
          downstream_context->run_resource_demand(
              additional_phase_retained_bytes(downstream_task_ids)));
    }
    return;
  }

  if (request.runtime) {
    IScheduler& dirty_task_runtime =
        ensure_running_scheduler(*request.runtime, request.intent);
    DirtyHandleTaskExecutor<RunTask> source_executor(
        compute_plan, request.selection, source_task_ids, run_task,
        dirty_task_runtime, false, SchedulerTaskPriority::High);
    DirtyHandleTaskExecutor<RunTask> downstream_executor(
        compute_plan, request.selection, downstream_task_ids, run_task,
        dirty_task_runtime, true, SchedulerTaskPriority::Normal);
    std::vector<int> initial_downstream_ids;
    if (request.selection) {
      initial_downstream_ids = request.selection->initial_downstream_task_ids;
    } else {
      TaskGraphReadyChecker ready_checker;
      initial_downstream_ids = ready_checker.initial_ready_task_ids(
          compute_plan.task_graph, &downstream_task_ids);
    }
    ComputeTaskDispatcher::submit_dirty_ready_tasks_source_first(
        dirty_task_runtime, source_executor.handles_for(source_task_ids),
        static_cast<int>(source_task_ids.size()),
        downstream_executor.handles_for(initial_downstream_ids),
        static_cast<int>(downstream_task_ids.size()),
        request.before_downstream);
    return;
  }

  for (int source_task_id : source_task_ids) {
    run_task(source_task_id);
  }
  if (request.before_downstream) {
    request.before_downstream();
  }
  TaskDependencyState dependency_state =
      request.selection
          ? TaskDependencyState(compute_plan.execution_order,
                                compute_plan.task_graph, downstream_task_ids,
                                request.selection->dependency_task_ids)
          : TaskDependencyState(compute_plan.execution_order,
                                compute_plan.task_graph, downstream_task_ids);
  std::vector<int> initial_downstream_ids;
  if (request.selection) {
    initial_downstream_ids = request.selection->initial_downstream_task_ids;
  } else {
    TaskGraphReadyChecker ready_checker;
    initial_downstream_ids = ready_checker.initial_ready_task_ids(
        compute_plan.task_graph, &downstream_task_ids);
  }
  std::vector<int> ready_stack(initial_downstream_ids.rbegin(),
                               initial_downstream_ids.rend());
  while (!ready_stack.empty()) {
    const int task_id = ready_stack.back();
    ready_stack.pop_back();
    run_task(task_id);
    std::vector<int> ready_ids = dependency_state.release_dependents(task_id);
    for (auto it = ready_ids.rbegin(); it != ready_ids.rend(); ++it) {
      ready_stack.push_back(*it);
    }
  }
}

}  // namespace ps::compute
