#pragma once

#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "graph_model.hpp"
#include "kernel/services/compute-service/compute_node_task_runner.hpp"
#include "kernel/services/compute-service/compute_task_dependency_state.hpp"

namespace ps {
class GraphTraversalService;
}  // namespace ps

namespace ps::compute {

/**
 * @brief Owns the scheduler-facing shape of one high-precision ComputePlan.
 *
 * TaskSubmissionPlan converts a cache-pruned ComputePlan into dense indexes,
 * dependency counters, dependent adjacency lists, scheduler closures, resolved
 * operation variants, and temporary result slots. It is the authority for
 * which nodes run during one ComputeTaskDispatcher::execute() call.
 *
 * @note The plan owns task closures that capture this object by reference.
 * Therefore the plan must remain alive until SchedulerTaskRuntime has drained
 * all submitted tasks.
 */
class TaskSubmissionPlan : public TaskExecutor {
 public:
  /**
   * @brief Builds scheduler submission state for one target node.
   *
   * @param graph GraphModel used for planning and operation resolution.
   * @param traversal Traversal service used by ComputeDispatchPlanBuilder.
   * @param node_id Target node id for the GlobalHighPrecision request.
   * @throws GraphError or standard exceptions from plan construction, graph
   * lookup, allocation, or operation resolution.
   * @note The underlying ComputePlan is recorded on GraphModel by the planning
   * unit before dependency state is constructed.
   */
  TaskSubmissionPlan(GraphModel& graph, GraphTraversalService& traversal,
                     int node_id);

  /**
   * @brief Reports whether the pruned plan contains no executable tasks.
   *
   * @return true when the task graph has no PlannedTask entries.
   * @throws Nothing.
   * @note An empty plan is legal only when the target already has reusable
   * high-precision output; dispatch treats an uncached empty target as a
   * planning contract error.
   */
  bool empty() const { return compute_plan_.task_graph.tasks.empty(); }

  /**
   * @brief Returns the number of planned executable tasks.
   *
   * @return Dense task count aligned with compute_plan_.task_graph.tasks.
   * @throws Nothing.
   */
  size_t size() const { return compute_plan_.task_graph.tasks.size(); }

  /**
   * @brief Returns the planned node id order.
   *
   * @return Const reference to dense planned node ids.
   * @throws Nothing.
   * @note The returned reference remains valid only while this plan object is
   * alive.
   */
  const std::vector<int>& execution_order() const { return execution_order_; }

  /**
   * @brief Returns the immutable compute plan used by this submission.
   *
   * @return Const reference to the cache-pruned plan.
   * @throws Nothing.
   * @note The reference remains valid only while this TaskSubmissionPlan lives.
   */
  const ComputePlan& compute_plan() const { return compute_plan_; }

  /**
   * @brief Returns the node id to dense index lookup.
   *
   * @return Const reference to the lookup map used by worker input resolution.
   * @throws Nothing.
   * @note The returned reference remains valid only while this plan object is
   * alive.
   */
  const std::unordered_map<int, int>& id_to_idx() const {
    return dependency_state_.id_to_idx();
  }

  /**
   * @brief Returns mutable temporary result slots for planned nodes.
   *
   * @return Reference to optional outputs aligned with execution_order_.
   * @throws Nothing.
   * @note Worker tasks publish into these slots; the result committer later
   * moves populated values into GraphModel under the graph mutex.
   */
  std::vector<std::optional<NodeOutput>>& temp_results() {
    return temp_results_;
  }

  /**
   * @brief Returns resolved operations for planned high-precision nodes.
   *
   * @return Const reference to optional operation variants aligned with
   * execution_order_.
   * @throws Nothing.
   * @note Missing operations are preserved as empty optionals so NodeTaskRunner
   * can throw a node-specific GraphError at execution time.
   */
  const std::vector<std::optional<OpRegistry::OpVariant>>& resolved_ops()
      const {
    return resolved_ops_;
  }

  /**
   * @brief Materializes scheduler closures for all planned nodes.
   *
   * @param runner Worker runner used by each closure.
   * @param task_runtime Scheduler runtime used for trace events, dependent task
   * submission, and completion accounting.
   * @throws std::bad_alloc if task closure storage allocation fails.
   * @note Each task captures this plan and runner by reference. Callers must
   * wait for scheduler completion before either object is destroyed.
   */
  void build_scheduler_tasks(NodeTaskRunner& runner,
                             SchedulerTaskRuntime& task_runtime);

  /**
   * @brief Moves ready initial scheduler task handles out of the plan.
   *
   * @return Initial task handles that may be submitted to
   * SchedulerTaskRuntime.
   * @throws std::bad_alloc if temporary ready-task storage grows.
   * @note TaskGraphReadyChecker is preferred. If the pruned graph carries no
   * initial ids, zero-dependency nodes are used as a compatibility fallback.
   */
  std::vector<TaskHandle> take_initial_task_handles();

  /**
   * @brief Executes one task handle selected by the scheduler.
   *
   * @param task_id Dense PlannedTask id from compute_plan_.task_graph.
   * @throws GraphError or operation exceptions from NodeTaskRunner.
   * @note This method is called by SchedulerTaskRuntime after epoch checks and
   * owns dependency release plus completion accounting for the task.
   */
  void run_task(int task_id) override;

  /**
   * @brief Emits trace events for tasks selected as initial scheduler work.
   *
   * @param task_runtime Scheduler runtime that receives AssignInitial events.
   * @throws Exceptions from task_runtime.log_event().
   * @note This is called after submit_initial_tasks() so traces reflect the
   * actual set of moved initial closures.
   */
  void log_initial_assignments(SchedulerTaskRuntime& task_runtime) const;

 private:
  /** @brief Resolves GlobalHighPrecision operation variants for planned nodes.
   */
  void resolve_operations();

  /** @brief Releases dependent tasks whose upstream counters reached zero. */
  void release_dependents(int current_task_id, int current_node_id,
                          SchedulerTaskRuntime& task_runtime);

  /** @brief Appends a task handle when all dependencies are satisfied. */
  void append_initial_task_handle(int task_id);

  /** @brief Appends initial tasks identified by the planned task graph. */
  void append_graph_ready_tasks();

  /** @brief Appends zero-dependency nodes as compatibility initial work. */
  void append_zero_dependency_tasks();

  /** @brief Builds one lightweight handle for a planned task id. */
  TaskHandle make_handle(int task_id) const;

  /** @brief Borrowed graph used for operation resolution and errors. */
  GraphModel& graph_;

  /** @brief Cache-pruned compute plan recorded for inspection and scheduling.
   */
  ComputePlan compute_plan_;

  /** @brief Dense planned node ids after cache pruning. */
  std::vector<int> execution_order_;

  /** @brief Runtime dependency counters and dense node-id mapping. */
  TaskDependencyState dependency_state_;

  /** @brief Scheduler handles aligned with task_graph.tasks. */
  std::vector<TaskHandle> task_handles_;

  /** @brief Initial task handles moved out for submit_initial_task_handles().
   */
  std::vector<TaskHandle> initial_task_handles_;

  /** @brief Task ids already selected as initial tasks. */
  std::unordered_set<int> submitted_initial_task_ids_;

  /** @brief Runner borrowed after build_scheduler_tasks() binds execution. */
  NodeTaskRunner* runner_ = nullptr;

  /** @brief Scheduler runtime borrowed for logging and dependency release. */
  SchedulerTaskRuntime* task_runtime_ = nullptr;

  /** @brief Temporary worker outputs aligned with execution_order_. */
  std::vector<std::optional<NodeOutput>> temp_results_;

  /** @brief Resolved operation variants aligned with execution_order_. */
  std::vector<std::optional<OpRegistry::OpVariant>> resolved_ops_;
};

/**
 * @brief Submits the planned scheduler work for one dispatch request.
 *
 * @param graph GraphModel being computed.
 * @param task_runtime Scheduler runtime receiving initial planned tasks.
 * @param node_id Target node id used when validating empty plans.
 * @param plan Built scheduler submission plan.
 * @throws GraphError when planning yields no scheduler tasks and the target
 * does not already have a reusable high-precision output. It may also rethrow
 * task submission or scheduler completion exceptions.
 * @note Empty plans are legal only when the target output is already present.
 * The dispatcher no longer falls back to the legacy recursive HP path.
 */
void dispatch_planned_tasks(GraphModel& graph,
                            SchedulerTaskRuntime& task_runtime, int node_id,
                            TaskSubmissionPlan& plan);

/**
 * @brief Submits dirty ready tasks with source-before-downstream ordering.
 *
 * @param task_runtime Scheduler runtime that executes and records task errors.
 * @param source_tasks Dirty source tasks to run with high priority.
 * @param downstream_tasks Dependent tasks to run with normal priority after
 * all sources and the optional boundary check complete.
 * @param epoch Optional scheduler epoch passed through to task submission.
 * @param before_downstream Optional boundary validation callback.
 * @throws Rethrows any task or callback exception after recording it in
 * task_runtime.
 * @note Source tasks may run concurrently, but downstream tasks are submitted
 * one at a time to preserve deterministic ordering at dirty update call sites.
 */
void submit_dirty_ready_tasks_source_first(
    SchedulerTaskRuntime& task_runtime,
    std::vector<SchedulerTaskRuntime::Task>&& source_tasks,
    std::vector<SchedulerTaskRuntime::Task>&& downstream_tasks,
    std::optional<uint64_t> epoch, std::function<void()> before_downstream);

}  // namespace ps::compute
