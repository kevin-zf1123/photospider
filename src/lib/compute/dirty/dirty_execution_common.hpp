#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "compute/dirty/dirty_region_planner.hpp"
#include "compute/dispatch/compute_task_dependency_state.hpp"
#include "compute/dispatch/compute_task_dispatcher.hpp"
#include "compute/dispatch/task_graph_planning.hpp"
#include "compute/execution/execution_service.hpp"
#include "compute/execution/resource_demand_estimator.hpp"
#if defined(PHOTOSPIDER_INTERNAL_DIRTY_UPDATE_TESTING)
#include "compute/dirty/dirty_update_executor_test_access.hpp"
#endif
#include "core/ps_types.hpp"      // NOLINT(build/include_subdir)
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {
class GraphModel;
class GraphRuntime;
class ExecutionHostContext;
}  // namespace ps

namespace ps::compute {

/**
 * @brief Copies one task node's current dirty staging output.
 * @param context Borrowed request state supplied with the callback.
 * @param node_id Planned graph node whose output is observed.
 * @return Mutex-protected output snapshot, or nullopt before publication.
 * @throws Any staging-copy exception unchanged.
 * @note The callback grants observation only. It must not wait, map payload,
 * mutate staging, or derive authorization from the returned output.
 */
using DirtyTaskOutputSnapshot = std::optional<NodeOutput> (*)(const void*, int);

/**
 * @brief Owns per-node critical sections for one dirty update transaction.
 *
 * Dirty ready callbacks use these mutexes while copying a live `Node`,
 * resolving its format-neutral runtime parameters, and touching request-local
 * staging metadata. A route-backed `RealTimeUpdate` shares one instance
 * between its concurrent HP and RT siblings so both domains synchronize access
 * to the same live node without coupling their task graphs or output buffers.
 *
 * @throws std::bad_alloc If node-id or mutex storage cannot be allocated.
 * @throws GraphError If checked retained-memory estimation overflows.
 * @note The node map is fully constructed before execution submission and is
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
   * caller must keep this object alive until every ready callback drains.
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
 * @brief Bounded dirty planning result used by HP and RT executors.
 *
 * The prepared state packages the graph-scoped dirty snapshot, the
 * immutable complete request-cone compute plan, the generation-local dirty
 * selection overlay with external-boundary demand cuts, and the materialized
 * source/downstream task groups that will be submitted to the selected physical
 * execution domain. Planning-time formal cache observations remain diagnostic
 * merge-base facts; the dirty plan itself owns the per-node HP or RT ROI
 * entries used by node execution.
 *
 * @tparam DirtyPlan HighPrecisionDirtyPlan or RealTimeDirtyPlan.
 * @note The struct is request-local. It must not be stored after execution
 * callbacks derived from it have drained.
 */
template <typename DirtyPlan>
struct PreparedDirtyPlan {
  /** @brief Dirty planner output with per-node execution entries. */
  DirtyPlan dirty_plan;

  /** @brief Complete request-cone compute plan used as immutable task shape. */
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
 * Injected runtime batches allocate their own epochs; process-service batches
 * use Run identity. Dirty generation remains separate snapshot and
 * source-commit provenance.
 */
struct DirtySourceFirstRunRequest {
  /** @brief Optional Graph runtime owning route bindings and trace storage. */
  GraphRuntime* runtime = nullptr;

  /** @brief Intent whose physical execution domain receives the work. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;

  /**
   * @brief Injected process execution service, or null for inline routing.
   */
  ExecutionService* execution_service = nullptr;

  /** @brief Exact private route used by every service batch in this request. */
  std::string execution_type = "cpu";

  /**
   * @brief Observation target borrowed for process-service settlement.
   *
   * @note Required exactly when execution_service is non-null and valid until
   * every synchronous service batch in this request has drained.
   */
  ExecutionHostContext* host = nullptr;

  /**
   * @brief Single-domain Run owning service submission leases.
   *
   * @note Required exactly when execution_service is non-null.
   */
  ComputeRun* run = nullptr;

  /**
   * @brief Existing lifecycle lease copied into phase/callback ownership.
   * @note ComputeService supplies this before attaching request cancellation,
   * eliminating terminal-time lease acquisition races. Direct private callers
   * may omit it and retain the pre-cancellation acquisition behavior.
   */
  const ComputeRunLease* run_lease = nullptr;

  /** @brief Compute plan containing immutable dirty task graph metadata. */
  const ComputePlan* compute_plan = nullptr;

  /** @brief Dirty generation-local active task view for dependency release. */
  const DirtyTaskSelectionOverlay* selection = nullptr;

  /**
   * @brief Selected devices aligned with `compute_plan->task_graph.tasks`.
   * @note Required for every request. Process-service submissions use the
   * value to enter the matching private CPU or GPU lane; inline execution
   * retains the same immutable planning snapshot without queue routing.
   */
  const std::vector<Device>* task_devices = nullptr;

  /**
   * @brief Operation start constraints aligned with planned task ids.
   * @note Required for every request. Missing operations use the all-default
   * value and fail authoritatively in node execution before provider entry.
   */
  const std::vector<OperationExecutionConstraints>* task_constraints = nullptr;

  /**
   * @brief Component-wise maximum operation demand for any task in this Run.
   * @note The source-first adapter adds its owned callback demand before
   * admission; operation retained and scratch bytes are not structural context
   * storage and therefore remain per-task charges.
   */
  ReadyTaskResourceDemand task_operation_resource_demand;

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

  /** @brief Borrowed context passed to snapshot_task_output. */
  const void* task_output_context = nullptr;

  /**
   * @brief Optional staging observer used for readiness continuation.
   * @note Process-service and inline dirty routes use the same callback. A
   * Pending image is continued only by a process service; inline execution
   * rejects it explicitly without blocking.
   */
  DirtyTaskOutputSnapshot snapshot_task_output = nullptr;

  /**
   * @brief Computes phase-local retained bytes immediately before admission.
   *
   * @note The callback receives the exact source or downstream task ids for
   * that synchronous segment after its owned context has been constructed.
   * Before a downstream callback runs, the source-first adapter explicitly
   * clears the moved-from outer task function so only the context-owned target
   * remains live. The callback may inspect request-owned staging state to
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
   *       runtime epoch or service Run id; injected runtime initial batches
   *       allocate independent local epochs.
   */
  uint64_t dirty_generation = 0;

  /** @brief Boundary validation invoked between source and downstream groups.
   */
  std::function<void()> before_downstream;
};

struct PreparedDirtySourceFirstRunState;

/**
 * @brief Move-only unpublished source/downstream dirty physical batches.
 *
 * The preparation owns every source/downstream context, ready submission,
 * ready-store node, root reservation, and dependency bootstrap needed by one
 * dirty domain. It publishes no service-ready work until execute() is called
 * after lifecycle installation.
 *
 * @throws Nothing from movement and destruction.
 * @note Destruction before execution rolls back every prepared root and
 * cancellation registration. The value has no lifecycle-admission authority.
 */
class PreparedDirtySourceFirstRun final {
 public:
  /** @brief Creates an inactive moved-from preparation. */
  PreparedDirtySourceFirstRun() noexcept;

  /**
   * @brief Transfers complete dirty publication ownership.
   * @param other Preparation made inactive.
   * @throws Nothing.
   */
  PreparedDirtySourceFirstRun(PreparedDirtySourceFirstRun&& other) noexcept;

  /**
   * @brief Replaces only an inactive preparation by transfer.
   * @param other Preparation made inactive.
   * @return Reference to this value.
   * @throws Nothing; replacing active ownership terminates.
   */
  PreparedDirtySourceFirstRun& operator=(
      PreparedDirtySourceFirstRun&& other) noexcept;

  /**
   * @brief Releases unpublished dirty phases.
   * @throws Nothing; trusted cleanup failure terminates.
   */
  ~PreparedDirtySourceFirstRun() noexcept;

  /** @brief Prevents duplicate reservation/publication ownership. */
  PreparedDirtySourceFirstRun(const PreparedDirtySourceFirstRun&) = delete;

  /** @brief Prevents duplicate reservation/publication assignment. */
  PreparedDirtySourceFirstRun& operator=(const PreparedDirtySourceFirstRun&) =
      delete;

  /**
   * @brief Reports whether this value owns one unexecuted preparation.
   * @return True until movement or execute().
   * @throws Nothing.
   */
  bool active() const noexcept { return state_ != nullptr; }

  /**
   * @brief Publishes and drains source then downstream work exactly once.
   * @return Nothing.
   * @throws GraphError or standard exceptions from cancellation, operation,
   * boundary validation, dependency release, or physical settlement.
   * @note The caller must install the matching lifecycle bundle before entry.
   */
  void execute();

 private:
  friend PreparedDirtySourceFirstRun prepare_dirty_source_first(
      const DirtySourceFirstRunRequest&, std::function<void(int)>,
      std::uint64_t);

  /**
   * @brief Owns one complete dirty preparation.
   * @param state Complete unpublished state.
   * @throws Nothing.
   */
  explicit PreparedDirtySourceFirstRun(
      std::unique_ptr<PreparedDirtySourceFirstRunState> state) noexcept;

  /** @brief Complete unpublished source/downstream state. */
  std::unique_ptr<PreparedDirtySourceFirstRunState> state_;
};

/**
 * @brief Prepares all dirty source/downstream work without publication.
 *
 * @param request Complete dirty route, plan, resource, and boundary inputs.
 * @param run_task Type-erased task callable retained by prepared contexts.
 * @param run_task_capture_bytes Audited concrete callable capture size before
 * type erasure.
 * @return Move-only preparation containing every physical root and ready node.
 * @throws GraphError or standard exceptions from validation, dependency
 * construction, resource estimation/reservation, and staging.
 * @throws std::bad_alloc unchanged from any preparation allocation.
 * @note For process-service routing both phase reservations remain live
 * together until execution/rollback, so lifecycle installation observes the
 * complete dirty-domain resource ownership rather than a later phase-local
 * admission. Each phase freezes its complete context estimate before
 * materialization moves already-charged constraint-key allocations into the
 * unique ready submissions.
 */
PreparedDirtySourceFirstRun prepare_dirty_source_first(
    const DirtySourceFirstRunRequest& request,
    std::function<void(int)> run_task, std::uint64_t run_task_capture_bytes);

/**
 * @brief Heap-owned dirty phase context for process-service submissions.
 *
 * The context owns a copy of the immutable compute plan, optional dirty
 * selection, active task membership, dependency counters, task callable, and
 * one matching Run lease. Each materialized `ReadyTaskSubmission` retains this
 * context and another matching lease, so no stack `ExecutionTaskExecutor*`
 * crosses the process-service boundary.
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
   * @param task_devices Selected devices aligned with compute-plan task ids.
   * @param task_constraints Operation gates aligned with compute-plan task ids.
   * @param task_operation_resource_demand Uniform component-wise maximum
   * operation demand for this physical Run.
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
   * @param task_output_context Borrowed request state for output snapshots.
   * @param snapshot_task_output Optional mutex-protected output observer.
   * @throws std::invalid_argument if run_task is empty.
   * @throws std::bad_alloc from owned state construction.
   */
  DirtyReadyTaskContext(
      const ComputePlan& compute_plan,
      const DirtyTaskSelectionOverlay* selection,
      const std::vector<int>& active_task_ids,
      const std::vector<Device>& task_devices,
      const std::vector<OperationExecutionConstraints>& task_constraints,
      ReadyTaskResourceDemand task_operation_resource_demand,
      std::function<void(int)> run_task,
      std::uint64_t run_task_retained_memory_bytes, ComputeRunLease lease,
      bool release_dependents, ExecutionTaskPriority priority,
      const void* task_output_context = nullptr,
      DirtyTaskOutputSnapshot snapshot_task_output = nullptr);

  /**
   * @brief Installs cancellation cleanup after shared ownership exists.
   * @return Nothing.
   * @throws std::bad_alloc or synchronization exceptions from Run callback
   * registration.
   * @note The callback captures only a weak context and cancels every pending
   * fence wait without retaining a callback-owner cycle.
   */
  void install_cancellation_notification();

  /**
   * @brief Cancels any unpublished fence continuation during destruction.
   * @throws Nothing; impossible completion-accounting failure terminates.
   */
  ~DirtyReadyTaskContext() noexcept;

  /**
   * @brief Estimates complete context-owned structural storage.
   * @return Checked copied plan/selection/dependency/callable/context bytes.
   * @throws GraphError when checked structural arithmetic overflows.
   * @note Every copied task-constraint key is charged by its actual string
   * capacity plus the null terminator. Phase admission freezes this estimate
   * before active entries move into their one ready submission, so the same
   * allocation remains charged exactly once. The shared Run control is
   * intentionally excluded and added once by `run_resource_demand()`.
   * Borrowed Graph/cache/staging references and opaque output payloads are
   * excluded.
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
   * identity validation only. Each active task may be materialized once; its
   * exact-identity constraint storage moves from this already-admitted context
   * into the submission without creating an unaccounted string copy.
   */
  std::vector<ReadyTaskSubmission> make_submissions(
      const std::vector<int>& task_ids, bool initial_ready);

 private:
  /** @brief Exact-once lifecycle of one active dirty task. */
  enum class TaskState : std::uint8_t {
    /** @brief No matching callback has entered. */
    Pending = 0U,
    /** @brief Original provider callback is executing. */
    Executing = 1U,
    /** @brief A ReadyFence wait owns deferred completion. */
    AwaitingValue = 2U,
    /** @brief The fence callback owns validation and release. */
    CompletingValue = 3U,
    /** @brief Task completed or terminal cancellation suppressed release. */
    Completed = 4U,
    /** @brief Task or continuation failed authoritatively. */
    Failed = 5U,
  };

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
   * @note Cancellation before provider entry or immediately after provider
   * return retires this callback's logical completion unit and returns without
   * releasing dependents. Cancellation during dependent publication stops the
   * remaining submissions before the same exact-once completion retirement.
   */
  void execute(ComputeRunLease& lease, const ComputeRunTaskIdentity& identity,
               ExecutionTaskRuntime& task_runtime);

  /**
   * @brief Defers one task whose exact staged named Value remains Pending.
   * @param task Planned task whose provider returned.
   * @param lease Matching Run observer.
   * @param task_runtime Runtime providing non-inline fence continuation.
   * @return True when a continuation owns an added completion unit.
   * @throws GraphError, ReadyFenceAccessError, or allocation/runtime errors
   * from output validation and wait registration.
   * @note Ready or absent tiled staging returns false. Canonical image and
   * generic names are scanned together; Failed and ProducerCancelled fail
   * closed. The exact pending name and Value identities are captured before
   * registration. Continuations chain remaining Pending names in canonical
   * order, and every declared Value must be Ready before release.
   */
  bool defer_pending_value(const PlannedTask& task, ComputeRunLease& lease,
                           ExecutionTaskRuntime& task_runtime);

  /**
   * @brief Completes one pending dirty Value after terminal fence delivery.
   * @param task_id Exact planned task left in AwaitingValue.
   * @param expected_name Exact pending output name captured at registration.
   * @param expected_value Immutable pending Value captured at registration.
   * @param record Exact added completion-unit ownership record.
   * @param snapshot Terminal fence observation delivered asynchronously.
   * @return Nothing after same-identity validation and either the next
   * Pending-name registration or dependent release.
   * @throws ReadyFenceAccessError for producer failure/cancellation.
   * @throws GraphError or runtime exceptions from identity, authority,
   * cancellation, or dependent publication.
   * @note The method retires the current dynamic completion unit exactly once
   * on all paths, adds a replacement unit before chaining, and closes sibling
   * waits before rethrowing a failure.
   */
  struct DeferredValueWait;
  void complete_deferred_value(int task_id, std::string expected_name,
                               Value expected_value, DeferredValueWait* record,
                               ReadyFenceSnapshot snapshot);

  /**
   * @brief Releases ready dependents after a task is authoritatively complete.
   * @param task Completed planned task.
   * @param lease Matching Run lease copied into dependent submissions.
   * @param task_runtime Active ready-submission runtime.
   * @return Nothing.
   * @throws Dependency, allocation, cancellation, or submission exceptions.
   */
  void release_task_dependents(const PlannedTask& task, ComputeRunLease& lease,
                               ExecutionTaskRuntime& task_runtime);

  /**
   * @brief Resolves one task node's frozen output authority.
   * @param task Planned task naming the graph node.
   * @return Borrowed exact authority from compute_plan_.
   * @throws GraphError when the retained plan is incomplete.
   */
  const PlannedOutputAuthority& output_authority_for(
      const PlannedTask& task) const;

  /**
   * @brief Closes dependent publication and cancels every pending fence wait.
   * @return Nothing.
   * @throws Nothing; completion-accounting corruption terminates.
   * @note The operation is idempotent and cancellation-safe. Registrations are
   * moved out of the publication gate before cancellation. A wait cancelled
   * before callback entry abandons its added logical unit instead of invoking
   * the worker-only decrement API from the cancelling thread; the exact Run
   * failure/cancellation terminal makes that count irrelevant, while retained
   * fence-executor ownership still prevents settlement until queued callback
   * state is destroyed or drained.
   */
  void close_publication() noexcept;

  /** @brief Immutable task shape copied into Run-phase ownership. */
  ComputePlan compute_plan_;

  /** @brief Optional copied dirty dependency and readiness overlay. */
  std::optional<DirtyTaskSelectionOverlay> selection_;

  /** @brief Exact active ids retained for dependency-state construction. */
  std::vector<int> active_task_ids_;

  /** @brief Selected devices aligned with dense compute-plan task ids. */
  std::vector<Device> task_devices_;

  /**
   * @brief Operation gates aligned with dense compute-plan task ids.
   * @note Each active entry moves into its one service submission only after
   * the context-owned retained-memory estimate is frozen.
   */
  std::vector<OperationExecutionConstraints> task_constraints_;

  /** @brief Uniform operation retained/scratch demand for every task. */
  ReadyTaskResourceDemand task_operation_resource_demand_;

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
  ExecutionTaskPriority priority_ = ExecutionTaskPriority::Normal;

  /** @brief Borrowed request object passed to snapshot_task_output_. */
  const void* task_output_context_ = nullptr;

  /** @brief Optional mutex-protected dirty output snapshot callback. */
  DirtyTaskOutputSnapshot snapshot_task_output_ = nullptr;

  /** @brief Exact-once task lifecycle states guarded by publication_mutex_. */
  std::vector<std::uint8_t> task_states_;

  /** @brief Serializes continuation registration, completion, and closure. */
  std::recursive_mutex publication_mutex_;

  /** @brief True after cancellation or failure forbids dependent release. */
  bool publication_closed_ = false;

  /** @brief Pending wait records aligned with dense planned task ids. */
  std::vector<std::unique_ptr<DeferredValueWait>> deferred_value_waits_;

  /** @brief Run cancellation callback that closes pending wait ownership. */
  ComputeRunCancellationRegistration cancellation_registration_;

  /** @brief Prevents duplicate cancellation callback installation. */
  bool cancellation_notification_installed_ = false;
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
 * @param compute_plan Request plan being published. Ordinary full HP plans may
 * be cache-pruned; dirty plans retain the complete request cone.
 * @param selection Optional dirty overlay used to summarize active work.
 * @throws std::bad_alloc if summary history storage cannot grow.
 * @note Full plans are retained only as the latest inspection entry; repeated
 * history stores summaries to avoid copying large task graphs.
 */
void remember_compute_plan(
    GraphModel& graph, const ComputePlan& compute_plan,
    const DirtyTaskSelectionOverlay* selection = nullptr);

/**
 * @brief Scopes a full task graph to one request and its cache policy.
 *
 * @param graph Graph that supplies topology, node metadata, and cache state.
 * @param request Intent, target node, and dirty ROI for the request.
 * @param execution_order Topological order selected by dirty planning.
 * @param available_devices Canonical route inventory used for coherent
 * operation selection and task-shape expansion.
 * @return Request-scoped plan. Ordinary full HP may be pruned at reusable-cache
 * demand boundaries; deferred dirty selection retains the complete request
 * cone.
 * @throws GraphError from task graph expansion or pruning.
 * @note The returned plan is still domain-specific and contains no mixed HP/RT
 * task pool.
 */
ComputePlan prune_node_cache_task_graph(
    GraphModel& graph, const ComputeRequest& request,
    const std::vector<int>& execution_order,
    const std::vector<Device>& available_devices = {Device::CPU});

/**
 * @brief Applies dirty snapshot selection to a request-scoped plan.
 *
 * @param node_cache_plan Plan already scoped to the target. Dirty execution
 * supplies a retained complete request cone.
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
 * dependency failures deterministic across inline and queued execution.
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
 * @param runtime Optional runtime that owns the execution trace log.
 * @param node_id Node being executed.
 * @param dirty_source Whether the node is a source boundary or downstream
 * dirty work.
 * @throws Any exception propagated by GraphRuntime::log_event.
 * @note A null runtime preserves inline execution behavior by emitting no
 * execution trace entries.
 */
void log_dirty_node_execution(GraphRuntime* runtime, int node_id,
                              bool dirty_source);

/**
 * @brief Logs and skips stale dirty source generations.
 *
 * @param runtime Optional runtime for execution trace events.
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
 * @brief Infers image channels and data type for a new output plan.
 *
 * @param preferred Existing output preferred for the target intent.
 * @param image_inputs Ready image inputs for the node.
 * @param fallback Optional secondary output used as a final shape hint.
 * @return Pair of channel count and data type.
 * @throws std::invalid_argument when canonical Value element facts or channel
 * counts cannot cross the current tiled compatibility boundary.
 * @note Defaults to one FLOAT32 channel when neither output nor input carries
 * concrete image metadata, matching the pre-split dirty update behavior. No
 * compatibility ImageBuffer is inspected or retained.
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
 * @brief Validates exact Region routes at the task-population boundary.
 *
 * @param graph Request-local graph whose current operation keys are checked.
 * @param route_snapshot Callback-free routes frozen by Region planning.
 * @param compute_plan Task-population result selected under the current
 * registry generation and device inventory.
 * @param selection Active dirty task overlay after cache and external
 * satisfaction pruning.
 * @param request Intent and target associated with the same dirty request.
 * @return Nothing when no task is active after pruning or every active node
 * still matches its frozen route.
 * @throws GraphError with `GraphErrc::NoOperation` when intent, device
 * inventory, operation key, route presence, identity, callback shape, or
 * metadata differs.
 * @throws GraphError with `GraphErrc::ComputeError` when selection contains an
 * invalid task id.
 * @throws std::bad_alloc when temporary active-node or operation-key storage
 * cannot allocate.
 * @note An empty active selection returns before intent, device-inventory,
 * task-id, or node-route comparison because no planned operation can execute.
 * An empty route snapshot with active work is a route-presence mismatch and
 * fails closed. Otherwise inactive and externally satisfied nodes remain
 * ignored while every active node is checked. Validation runs before ROI
 * application, task materialization, callable resolution, resource
 * estimation, gate/grant construction, reservation, or provider entry.
 */
void validate_dirty_region_operation_routes(
    const GraphModel& graph,
    const DirtyRegionOperationRouteSnapshot& route_snapshot,
    const ComputePlan& compute_plan, const DirtyTaskSelectionOverlay& selection,
    const ComputeRequest& request);

/**
 * @brief Prepares common dirty execution state after planner output exists.
 *
 * @tparam DirtyPlan HighPrecisionDirtyPlan or RealTimeDirtyPlan.
 * @param graph Request-local graph used for task shape, planning-time cache
 * observation, and dirty selection. It may be a stabilized shadow graph.
 * @param dirty_plan Dirty planner output for one intent domain.
 * @param request Compute request matching the same dirty domain.
 * @param available_devices Canonical route inventory frozen for planning and
 * later exact-identity resolution.
 * @param externally_satisfied_node_ids Optional nodes already executed by
 * parameter stabilization and excluded from phase-two task selection.
 * @return Prepared plan with node groups ready for task construction.
 * @throws GraphError from task graph pruning, exact Region route validation,
 * or materialization. A changed route is reported as
 * `GraphErrc::NoOperation`.
 * @note The helper retains diagnostics without publishing them. It defers
 * reusable-cache demand cutting so the complete callback-free request cone is
 * retained, then applies dirty-candidate and external-boundary demand
 * selection. Exact cache may seed a dirty write buffer but cannot satisfy a
 * task selected by the snapshot. The installed execution path calls
 * publish_prepared_dirty_inspection() only after its first cancellation
 * observation, so candidate rollback leaves authoritative inspection state
 * unchanged. A no-work selection returns successfully from route validation
 * before comparing frozen execution context. Any active work still requires
 * full context and route validation before ROI mutation, work-set
 * materialization, callable resolution, and every admission/resource boundary.
 */
template <typename DirtyPlan>
PreparedDirtyPlan<DirtyPlan> prepare_dirty_execution(
    GraphModel& graph, DirtyPlan&& dirty_plan, const ComputeRequest& request,
    const std::vector<Device>& available_devices = {Device::CPU},
    const std::unordered_set<int>* externally_satisfied_node_ids = nullptr) {
  ComputeRequest dirty_selection_request = request;
  dirty_selection_request.defer_reusable_cache_pruning = true;
  const ComputePlan node_cache_plan = prune_node_cache_task_graph(
      graph, dirty_selection_request, dirty_plan.execution_order,
      available_devices);
#if defined(PHOTOSPIDER_INTERNAL_DIRTY_UPDATE_TESTING)
  testing::notify_dirty_node_cache_plan_test_hook(node_cache_plan, graph);
#endif
  DirtySnapshotTaskGraphPruner dirty_snapshot_pruner;
  DirtyTaskSelectionOverlay selection =
      dirty_snapshot_pruner.select(node_cache_plan, dirty_plan.snapshot, graph,
                                   externally_satisfied_node_ids);
  validate_dirty_region_operation_routes(graph, dirty_plan.operation_routes,
                                         node_cache_plan, selection, request);
  apply_planned_work_rois(dirty_plan.entries, selection);

  DirtyUpdateWorkSet work_set = dirty_snapshot_pruner.materialize(selection);
  std::vector<int> source_task_ids = work_set.dirty_source_task_ids;
  std::vector<int> downstream_task_ids = work_set.downstream_task_ids;

  return PreparedDirtyPlan<DirtyPlan>{
      std::move(dirty_plan),      std::move(node_cache_plan),
      std::move(selection),       std::move(work_set),
      std::move(source_task_ids), std::move(downstream_task_ids)};
}

/**
 * @brief Publishes one installed dirty preparation for graph inspection.
 *
 * @tparam DirtyPlan HighPrecisionDirtyPlan or RealTimeDirtyPlan.
 * @param graph Authoritative live graph receiving diagnostics.
 * @param prepared Immutable prepared snapshot, plan, and active selection.
 * @return Nothing.
 * @throws std::bad_alloc when debug text or bounded histories grow.
 * @note The caller holds graph.graph_mutex_. Publication occurs after
 * lifecycle installation and the first execution-time cancellation check.
 * Failed installed execution therefore retains its planning evidence, while a
 * rejected candidate publishes nothing.
 */
template <typename DirtyPlan>
void publish_prepared_dirty_inspection(
    GraphModel& graph, const PreparedDirtyPlan<DirtyPlan>& prepared) {
  graph.last_dirty_region_snapshot_debug =
      DirtyRegionPlanner::describe_snapshot(prepared.dirty_plan.snapshot);
  remember_dirty_snapshot(graph, prepared.dirty_plan.snapshot);
  remember_compute_plan(graph, prepared.compute_plan, &prepared.selection);
}

/**
 * @brief Task-id backed ExecutionTaskExecutor used by dirty update task
 * handles.
 *
 * The executor exposes compact ExecutionTaskHandle entries and invokes one
 * request-local callable with the selected task id. For downstream dirty work
 * it also owns task-level TaskDependencyState so completed tasks release ready
 * dependents in batches.
 *
 * @note The executor is stack-owned by run_dirty_source_first() and must remain
 * alive until the matching ExecutionTaskRuntime::wait_for_completion() returns.
 * It does not allocate one std::function per active dirty task.
 */
template <typename RunTask>
class DirtyHandleExecutionTaskExecutor : public ExecutionTaskExecutor {
 public:
  /**
   * @brief Binds task runner, dependency state, and execution runtime.
   *
   * @param compute_plan Plan whose task graph owns immutable task metadata.
   * @param selection Optional dirty overlay with dependency overrides.
   * @param active_task_ids Task ids active in this source or downstream phase.
   * @param run_task Callable invoked with one active dirty task id.
   * @param task_runtime Runtime used for dependent handle submission.
   * @param run_lease Optional read-only lease for cooperative cancellation.
   * @param release_dependents Whether completed tasks should release
   * downstream dependents.
   * @param priority Priority used when dependency release submits ready work.
   * @throws std::bad_alloc if dependency state allocation fails.
   */
  DirtyHandleExecutionTaskExecutor(const ComputePlan& compute_plan,
                                   const DirtyTaskSelectionOverlay* selection,
                                   const std::vector<int>& active_task_ids,
                                   RunTask& run_task,
                                   ExecutionTaskRuntime& task_runtime,
                                   const ComputeRunLease* run_lease,
                                   bool release_dependents,
                                   ExecutionTaskPriority priority)
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
        run_lease_(run_lease ? std::optional<ComputeRunLease>(*run_lease)
                             : std::nullopt),
        release_dependents_(release_dependents),
        priority_(priority) {
    task_handles_.resize(compute_plan_.task_graph.tasks.size());
    for (const auto& task : compute_plan_.task_graph.tasks) {
      if (task.task_id < 0 ||
          task.task_id >= static_cast<int>(task_handles_.size())) {
        continue;
      }
      task_handles_[task.task_id] =
          ExecutionTaskHandle{this, task.task_id, task.node_id};
    }
  }

  /**
   * @brief Builds handles for selected task ids.
   *
   * @param task_ids Task ids to expose as execution handles.
   * @return Handles aligned with task_ids order, skipping invalid ids.
   * @throws std::bad_alloc if output allocation fails.
   */
  std::vector<ExecutionTaskHandle> handles_for(
      const std::vector<int>& task_ids) const {
    std::vector<ExecutionTaskHandle> handles;
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
   * @param task_id Dirty task id selected by the execution runtime.
   * @return Nothing.
   * @throws GraphError when cancellation is observed before or after the dirty
   * provider, or when the provider reports a graph-domain failure.
   * @throws Any other provider, trace, dependency, submission, or completion
   * exception unchanged.
   * @note Only the normal path retires the runtime completion unit here;
   * exception settlement remains owned by the execution runtime. Cancellation
   * after provider return prevents dependency release, while a provider already
   * entered is non-preemptible except at its own tile observations.
   */
  void run_task(int task_id) override {
    const auto& task = compute_plan_.task_graph.tasks.at(task_id);
    try {
      if (run_lease_ && run_lease_->observe_cancellation().has_value()) {
        throw GraphError(GraphErrc::ComputeError,
                         "ComputeRun cancelled before dirty task.");
      }
      run_task_(task_id);
      if (run_lease_ && run_lease_->observe_cancellation().has_value()) {
        throw GraphError(GraphErrc::ComputeError,
                         "ComputeRun cancelled after dirty task.");
      }
      if (release_dependents_) {
        std::vector<int> ready_ids =
            dependency_state_.release_dependents(task_id);
        task_runtime_.submit_ready_task_handles_from_worker(
            handles_for(ready_ids), priority_);
      }
    } catch (...) {
      task_runtime_.log_event(ExecutionTraceAction::RethrowException,
                              task.node_id);
      throw;
    }
    task_runtime_.dec_tasks_to_complete();
  }

 private:
  /** @brief Immutable request-cone compute plan whose selected tasks run. */
  const ComputePlan& compute_plan_;

  /** @brief Task-level dependency counters for this active phase. */
  TaskDependencyState dependency_state_;

  /** @brief Request-local dirty task runner called with a dense task id. */
  RunTask& run_task_;

  /** @brief Execution runtime borrowed for ready release and completion. */
  ExecutionTaskRuntime& task_runtime_;

  /** @brief Optional retained Run observer for callback boundaries. */
  std::optional<ComputeRunLease> run_lease_;

  /** @brief Ready handles aligned with task id. */
  std::vector<ExecutionTaskHandle> task_handles_;

  /** @brief Whether this executor releases downstream dependents. */
  bool release_dependents_ = false;

  /** @brief Priority used for ready dependent submissions. */
  ExecutionTaskPriority priority_ = ExecutionTaskPriority::Normal;
};

/**
 * @brief Constructs a dirty context before releasing its outer callable owner.
 *
 * @tparam CallableHolder Move source accepted by ContextFactory and resettable
 * through a non-throwing null assignment.
 * @tparam ContextFactory Factory that accepts CallableHolder as an rvalue and
 * returns an owning destination value.
 * @param callable_holder Outer callable owner transferred to the destination.
 * @param context_factory Factory that must finish destination construction
 * before the outer owner is released.
 * @return Owning destination returned by context_factory after the outer
 * holder has been explicitly cleared.
 * @throws Any exception propagated by context_factory or destination return
 * movement.
 * @note The null assignment is part of the successful construction boundary:
 * no caller code can run between destination construction and outer release.
 * If construction throws, the holder and every factory temporary unwind
 * through RAII; the outer holder remains valid but may be moved from. This
 * private source-tree helper adds no installed API or ABI.
 */
template <typename CallableHolder, typename ContextFactory>
auto make_dirty_context_and_release_outer_callable(
    CallableHolder& callable_holder, ContextFactory&& context_factory) {
  static_assert(noexcept(callable_holder = nullptr),
                "Dirty callable holder reset must not throw.");
  auto context =
      std::forward<ContextFactory>(context_factory)(std::move(callable_holder));
  callable_holder = nullptr;
  return context;
}

/**
 * @brief Runs dirty source tasks before downstream dirty tasks.
 *
 * @tparam RunTask Callable that executes one dirty task id.
 * @param request Source-first dispatch request and boundary validation.
 * @param run_task Task runner invoked with dense task ids.
 * @return Nothing.
 * @throws Exceptions from task construction, task execution, boundary
 * validation, runtime lookup, or execution submission.
 * @note Injected runtime execution delegates source-first submission to
 * ComputeTaskDispatcher::submit_dirty_ready_tasks_source_first. Process
 * service execution materializes heap-owned Run submissions. Its source
 * context copies the outer `std::function`, so source admission charges both
 * live callable targets until synchronous settlement. Downstream transfers
 * that outer target through
 * make_dirty_context_and_release_outer_callable(), which makes successful
 * context construction and explicit release of the valid-but-unspecified
 * moved-from function one boundary. Submission construction, retained-demand
 * calculation, and admission therefore see only the context-owned target
 * without depending on the standard library's moved-from representation. The
 * dirty executor retains request-local inline fallback ordering. Cancellation
 * is observed at phase entry, around every source/downstream provider and
 * boundary callback, and before dependency publication. It prevents later
 * phase entry and final staged publication; a monolithic callback already
 * entered remains non-preemptible until it returns.
 */
template <typename RunTask>
void run_dirty_source_first(const DirtySourceFirstRunRequest& request,
                            RunTask run_task) {
  prepare_dirty_source_first(request,
                             std::function<void(int)>(std::move(run_task)),
                             static_cast<std::uint64_t>(sizeof(RunTask)))
      .execute();
}

}  // namespace ps::compute
