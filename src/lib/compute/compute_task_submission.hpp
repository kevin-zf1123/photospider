#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "compute/compute_node_task_runner.hpp"
#include "compute/compute_run.hpp"
#include "compute/compute_task_dependency_state.hpp"
#include "compute/execution_service.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {
class GraphTraversalService;
}  // namespace ps

namespace ps::compute {

/**
 * @brief Owns the scheduler-facing shape of one high-precision ComputePlan.
 *
 * TaskSubmissionPlan converts a cache-pruned ComputePlan into dense indexes,
 * dependency counters, dependent adjacency lists, composite Run-local task
 * identity, an owned NodeTaskRunner, resolved operation variants, and
 * temporary result slots. It is the authority for which nodes run during one
 * ComputeTaskDispatcher::execute() call.
 *
 * @throws GraphError or standard exceptions through individual construction,
 * execution, and submission operations.
 * @note Scheduler callbacks never capture this object directly. They capture a
 * ComputeRunLease and reach the plan only through lease-validated identity.
 */
class TaskSubmissionPlan {
 public:
  /**
   * @brief Builds scheduler submission state for one target node.
   *
   * @param run_id Opaque namespace assigned by the owning ComputeRun.
   * @param graph GraphModel used for planning and operation resolution.
   * @param traversal Traversal service used by ComputeDispatchPlanBuilder.
   * @param node_id Target node id for the GlobalHighPrecision request.
   * @param available_devices Devices exposed by the active scheduler runtime.
   * @throws GraphError or standard exceptions from plan construction, graph
   * lookup, allocation, or operation resolution.
   * @note The underlying ComputePlan is recorded on GraphModel by the planning
   * unit before dependency state is constructed.
   */
  TaskSubmissionPlan(ComputeRunId run_id, GraphModel& graph,
                     GraphTraversalService& traversal, int node_id,
                     std::vector<Device> available_devices);

  /**
   * @brief Reports whether the pruned plan contains no executable tasks.
   *
   * @return true when the task graph has no PlannedTask entries.
   * @throws Nothing.
   * @note An empty plan is legal only when the target already has reusable
   * high-precision output.
   */
  bool empty() const { return compute_plan_.task_graph.tasks.empty(); }

  /**
   * @brief Returns the number of planned executable tasks.
   *
   * @return Dense task count aligned with compute_plan_.task_graph.tasks.
   * @throws Nothing.
   */
  std::size_t size() const { return compute_plan_.task_graph.tasks.size(); }

  /**
   * @brief Returns the planned node id order.
   *
   * @return Const reference to dense planned node ids.
   * @throws Nothing.
   * @note The reference remains valid only while this plan is lease-retained.
   */
  const std::vector<int>& execution_order() const { return execution_order_; }

  /**
   * @brief Returns the immutable compute plan used by this submission.
   *
   * @return Const reference to the cache-pruned plan.
   * @throws Nothing.
   * @note The reference remains valid only while this plan is lease-retained.
   */
  const ComputePlan& compute_plan() const { return compute_plan_; }

  /**
   * @brief Returns the node id to dense execution-index lookup.
   *
   * @return Const reference used by worker input resolution.
   * @throws Nothing.
   */
  const std::unordered_map<int, int>& id_to_idx() const {
    return dependency_state_.id_to_idx();
  }

  /**
   * @brief Returns mutable temporary result slots for planned nodes.
   *
   * @return Slots aligned with execution_order_.
   * @throws Nothing.
   * @note Workers publish here before the dispatcher serializes Graph commit.
   */
  std::vector<std::optional<NodeOutput>>& temp_results() {
    return temp_results_;
  }

  /**
   * @brief Returns resolved operations for planned high-precision nodes.
   *
   * @return Optional variants aligned with execution_order_.
   * @throws Nothing.
   * @note Missing operations remain empty for worker-time node diagnostics.
   */
  const std::vector<std::optional<OpRegistry::OpVariant>>& resolved_ops()
      const {
    return resolved_ops_;
  }

  /**
   * @brief Constructs and owns the worker runner for this Run plan.
   *
   * @param context Graph services, plan vectors, telemetry, and options
   * borrowed by the runner while leased callbacks execute.
   * @return Nothing.
   * @throws std::logic_error when a runner is already installed.
   * @throws std::bad_alloc when runner-owned synchronization storage cannot
   * allocate.
   * @note The runner lives in this Run-owned plan rather than on the dispatcher
   * stack.
   */
  void emplace_task_runner(NodeTaskRunnerContext context);

  /**
   * @brief Builds one composite identity in this plan's Run namespace.
   *
   * @param task_id Dense registered PlannedTask id.
   * @return Composite Run/local task identity.
   * @throws std::out_of_range when task_id is not registered.
   */
  ComputeRunTaskIdentity task_identity(int task_id) const;

  /**
   * @brief Checks whether a composite identity belongs to this plan.
   *
   * @param identity Candidate Run/local identity.
   * @return true only when Run id and dense local registration match.
   * @throws Nothing.
   */
  bool contains_task_identity(
      const ComputeRunTaskIdentity& identity) const noexcept;

  /**
   * @brief Publishes the full initial ready set as lease-backed callbacks.
   *
   * @param lease Matching lease copied into every accepted initial callback.
   * @param task_runtime Active scheduler batch receiving completion count,
   * trace, and owned callbacks.
   * @return Nothing.
   * @throws GraphError when a nonempty plan has no initial ready work.
   * @throws std::bad_alloc from ready identity or callback storage.
   * @throws Scheduler runtime exceptions from count, trace, or submission.
   * @note The caller owns one bootstrap completion unit. This method adds the
   * planned task count but does not release the bootstrap unit.
   */
  void submit_initial_ready_tasks(const ComputeRunLease& lease,
                                  SchedulerTaskRuntime& task_runtime);

  /**
   * @brief Materializes the initial dependency-ready set for ExecutionService.
   *
   * @param lease Matching Run lease copied into every ready submission.
   * @return Move-owned submissions carrying immutable metadata, executable,
   * composite identity, and matching leases.
   * @throws GraphError when a nonempty plan has no initial ready work.
   * @throws std::overflow_error when planned count exceeds scheduler integer
   * accounting.
   * @throws std::bad_alloc from ready identity, metadata, executable, or output
   * storage.
   * @note This method performs readiness discovery but no queue submission or
   * completion-count mutation. ExecutionService receives no task graph or
   * dependency map.
   */
  std::vector<ReadyTaskSubmission> make_initial_ready_submissions(
      const ComputeRunLease& lease);

  /**
   * @brief Executes one lease-validated planned task exactly once.
   *
   * @param identity Matching composite identity already checked by the lease.
   * @param lease Lease copied into newly released dependent callbacks.
   * @param task_runtime Active scheduler runtime for trace, submission, and
   * completion accounting.
   * @return Nothing.
   * @throws std::invalid_argument for mismatched identity.
   * @throws std::logic_error when the registered task already entered.
   * @throws GraphError or operation exceptions from NodeTaskRunner and
   * dependency release.
   * @note Success releases dependents and one scheduler completion unit.
   * Failure publication is owned by the calling lease route.
   */
  void execute_task(const ComputeRunTaskIdentity& identity,
                    const ComputeRunLease& lease,
                    SchedulerTaskRuntime& task_runtime);

 private:
  /**
   * @brief Exact-once entry state for one registered local task.
   *
   * @throws Nothing for atomic representation operations.
   */
  enum class TaskExecutionState : std::uint8_t {
    /** @brief Callback has not entered Run-local execution. */
    Pending = 0U,
    /** @brief One matching callback owns execution. */
    Executing = 1U,
    /** @brief Execution, dependent release, and completion succeeded. */
    Completed = 2U,
    /** @brief Execution or completion routing threw. */
    Failed = 3U,
  };

  /**
   * @brief Resolves GlobalHighPrecision operation variants for planned nodes.
   *
   * @return Nothing.
   * @throws std::bad_alloc from registry candidate or result storage.
   * @note Missing variants remain empty for worker-time GraphError context.
   */
  void resolve_operations();

  /**
   * @brief Releases dependent tasks whose upstream counters reached zero.
   *
   * @param current_task_id Completed task whose dependency edges advance.
   * @param current_node_id Completed node used in scheduling diagnostics.
   * @param lease Matching lease copied into dependent callbacks.
   * @param task_runtime Runtime receiving newly ready owned callbacks.
   * @return Nothing.
   * @throws std::bad_alloc if ready identity/callback submission exhausts
   * memory.
   * @throws GraphError with scheduling-stage context for other dependency,
   * range, or scheduler submission failures.
   * @note Resource exhaustion keeps its exception identity; recoverable
   * scheduling failures receive node context.
   */
  void release_dependents(int current_task_id, int current_node_id,
                          const ComputeRunLease& lease,
                          SchedulerTaskRuntime& task_runtime);

  /**
   * @brief Discovers and validates the full initial ready identity set.
   *
   * @return Composite identities in this plan's Run namespace.
   * @throws GraphError when a nonempty plan has no initial ready work.
   * @throws std::overflow_error when planned count exceeds scheduler integer
   * accounting.
   * @throws std::bad_alloc or std::out_of_range from ready discovery.
   * @note The method resets only initial-submission deduplication state; it
   * mutates no dependency counter or scheduler state.
   */
  std::vector<ComputeRunTaskIdentity> initial_ready_identities();

  /**
   * @brief Builds one owned service submission for a registered ready task.
   *
   * @param lease Matching Run lease copied into submission ownership.
   * @param identity Registered composite task identity.
   * @param is_initial_ready Whether initial discovery selected the task.
   * @return Move-owned ready submission.
   * @throws std::out_of_range when identity does not name a registered task.
   * @throws std::invalid_argument for an unexpected lease/identity mismatch.
   * @throws std::bad_alloc from metadata or executable ownership.
   * @note The executable captures no plan, runner, Graph, or dispatcher stack
   * pointer; it reaches Run-owned state only through the supplied lease.
   */
  ReadyTaskSubmission make_ready_submission(
      const ComputeRunLease& lease, const ComputeRunTaskIdentity& identity,
      bool is_initial_ready);

  /**
   * @brief Appends one initially ready composite identity.
   *
   * @param task_id Dense candidate task id.
   * @param identities Output ready identity list.
   * @return Nothing.
   * @throws std::out_of_range for inconsistent dense task metadata.
   * @throws std::bad_alloc if output or deduplication storage grows.
   */
  void append_initial_task_identity(
      int task_id, std::vector<ComputeRunTaskIdentity>& identities);

  /**
   * @brief Appends graph-declared initial ready identities.
   *
   * @param identities Output ready identity list.
   * @return Nothing.
   * @throws std::bad_alloc or std::out_of_range from ready discovery.
   */
  void append_graph_ready_tasks(
      std::vector<ComputeRunTaskIdentity>& identities);

  /**
   * @brief Appends zero-dependency identities as compatibility fallback.
   *
   * @param identities Output ready identity list.
   * @return Nothing.
   * @throws std::bad_alloc or std::out_of_range from ready discovery.
   */
  void append_zero_dependency_tasks(
      std::vector<ComputeRunTaskIdentity>& identities);

  /**
   * @brief Emits traces for task ids selected as initial work.
   *
   * @param task_runtime Runtime receiving AssignInitial events.
   * @return Nothing.
   * @throws Exceptions from task_runtime.log_event().
   */
  void log_initial_assignments(SchedulerTaskRuntime& task_runtime) const;

  /** @brief Opaque namespace retained from the owning Run descriptor. */
  ComputeRunId run_id_;

  /** @brief Borrowed graph used for operation resolution and diagnostics. */
  GraphModel& graph_;

  /** @brief Cache-pruned compute plan recorded for scheduling/inspection. */
  ComputePlan compute_plan_;

  /** @brief Dense planned node ids after cache pruning. */
  std::vector<int> execution_order_;

  /** @brief Devices available for operation implementation selection. */
  std::vector<Device> available_devices_;

  /** @brief Runtime dependency counters and dense node-id mapping. */
  TaskDependencyState dependency_state_;

  /** @brief Task ids already selected as initial work. */
  std::unordered_set<int> submitted_initial_task_ids_;

  /** @brief Exact-once state aligned with composite local task ids. */
  std::vector<std::atomic<std::uint8_t>> task_execution_states_;

  /** @brief Run-owned worker runner retained by callback leases. */
  std::unique_ptr<NodeTaskRunner> task_runner_;

  /** @brief Temporary worker outputs aligned with execution_order_. */
  std::vector<std::optional<NodeOutput>> temp_results_;

  /** @brief Resolved operations aligned with execution_order_. */
  std::vector<std::optional<OpRegistry::OpVariant>> resolved_ops_;
};

/**
 * @brief Submits one planned full-HP graph through lease-backed callbacks.
 *
 * @param graph GraphModel used to validate target cache state for empty plans.
 * @param task_runtime Runtime receiving one empty epoch batch and owned
 * callbacks.
 * @param node_id Target node id used in empty-plan diagnostics.
 * @param plan Run-owned scheduler submission plan.
 * @param dispatcher_lease Lease retained by dispatcher/commit and copied into
 * the bootstrap callback.
 * @return Nothing.
 * @throws GraphError when an empty plan lacks reusable target output.
 * @throws Scheduler or task exceptions unchanged through completion wait.
 * @note The full HP path submits no non-empty TaskHandle and exposes no
 * borrowed TaskExecutor pointer. Empty plans bypass batch setup.
 */
void dispatch_planned_tasks(GraphModel& graph,
                            SchedulerTaskRuntime& task_runtime, int node_id,
                            TaskSubmissionPlan& plan,
                            const ComputeRunLease& dispatcher_lease);

/**
 * @brief Submits one planned full-HP Run through the injected CPU service.
 *
 * @param graph GraphModel used only to validate an empty reusable target.
 * @param execution_service Process-owned CPU execution service.
 * @param host Active Graph scheduler observation context.
 * @param node_id Target node used in empty-plan diagnostics.
 * @param plan Run-owned scheduler submission plan.
 * @param dispatcher_lease Matching lease copied into ready submissions.
 * @return Nothing after the service batch settles.
 * @throws GraphError when an empty plan lacks reusable target output.
 * @throws std::bad_alloc from submission/service setup.
 * @throws The exact service worker or operation exception after settlement.
 * @note Dispatcher readiness and completion state stay inside plan/Run
 * ownership. Only move-owned ready submissions cross into the service.
 */
void dispatch_planned_tasks(GraphModel& graph,
                            ExecutionService& execution_service,
                            SchedulerHostContext& host, int node_id,
                            TaskSubmissionPlan& plan,
                            const ComputeRunLease& dispatcher_lease);

}  // namespace ps::compute
