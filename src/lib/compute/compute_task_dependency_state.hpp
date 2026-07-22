#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "compute/task_graph_planning.hpp"

namespace ps::compute {

/**
 * @brief Dense runtime dependency state for a pruned ComputeTaskGraph.
 *
 * TaskDependencyState converts PlannedTask dependency_task_ids into compact
 * execution counters and dependent adjacency lists. It owns only per-dispatch
 * runtime bookkeeping; the immutable ComputeTaskGraph remains owned by the
 * submission plan.
 *
 * @note The state is not thread-safe for topology mutation. Worker threads may
 * call release_dependents() after tasks are built, while the owning
 * TaskSubmissionPlan keeps the object alive until execution completion.
 */
class TaskDependencyState {
 public:
  /**
   * @brief Builds dense dependency counters from planned tasks.
   *
   * @param execution_order Dense planned node ids produced by the pruned plan.
   * @param task_graph Task graph containing PlannedTask dependency metadata.
   * @throws std::bad_alloc if counter, map, or adjacency storage cannot grow.
   * @note All tasks in task_graph are active for this constructor.
   */
  TaskDependencyState(const std::vector<int>& execution_order,
                      const ComputeTaskGraph& task_graph);

  /**
   * @brief Builds dependency state for an explicit active task subset.
   *
   * @param execution_order Dense planned node ids produced by the pruned plan.
   * @param task_graph Task graph containing PlannedTask dependency metadata.
   * @param active_task_ids Task ids that participate in this runtime view.
   * @throws std::bad_alloc if counter, map, or adjacency storage cannot grow.
   * @note Dependencies outside active_task_ids are treated as already
   * satisfied, which is how dirty source completion releases downstream work.
   */
  TaskDependencyState(const std::vector<int>& execution_order,
                      const ComputeTaskGraph& task_graph,
                      const std::vector<int>& active_task_ids);

  /**
   * @brief Builds dependency state for an active subset with dependency view.
   *
   * @param execution_order Dense planned node ids produced by the pruned plan.
   * @param task_graph Task graph containing task count and node metadata.
   * @param active_task_ids Task ids that participate in this runtime view.
   * @param dependency_task_ids Dependency ids aligned with task id.
   * @throws std::bad_alloc if counter, map, or adjacency storage cannot grow.
   * @note Dirty overlays use this constructor so snapshot ROI edge mappings can
   * affect ready release without copying PlannedTask objects.
   */
  TaskDependencyState(const std::vector<int>& execution_order,
                      const ComputeTaskGraph& task_graph,
                      const std::vector<int>& active_task_ids,
                      const std::vector<std::vector<int>>& dependency_task_ids);

  /**
   * @brief Returns the node id to dense execution-index map.
   *
   * @return Const reference used by worker input resolution.
   * @throws Nothing.
   * @note The reference remains valid only while this state object lives.
   */
  const std::unordered_map<int, int>& id_to_idx() const { return id_to_idx_; }

  /**
   * @brief Estimates dynamic Host-owned dependency-state storage.
   * @return Checked map buckets/nodes, counter, adjacency, and active-bit
   * bytes.
   * @throws GraphError when checked structural arithmetic overflows.
   * @note The result excludes `sizeof(TaskDependencyState)` so enclosing
   * owners can account the inline object exactly once.
   */
  std::uint64_t dynamic_retained_memory_bytes() const;

  /**
   * @brief Checks whether a task can be submitted as initial work.
   *
   * @param task_id Dense PlannedTask id being considered.
   * @return true when the task is active and has no remaining dependencies.
   * @throws std::out_of_range if task_id is not a valid dense task id.
   * @note Uses acquire ordering so initial submission observes dependency
   * counter initialization.
   */
  bool ready_for_initial_submit(int task_id) const;

  /**
   * @brief Releases dependent tasks after one upstream task completes.
   *
   * @param current_task_id Dense id of the completed PlannedTask.
   * @param submit_ready Callback invoked with each dependent task id whose
   * counter reaches zero.
   * @throws std::out_of_range for inconsistent dense indexes, or exceptions
   * propagated by submit_ready.
   * @note The caller is responsible for publishing produced outputs before
   * invoking this method.
   */
  void release_dependents(
      int current_task_id,
      const std::function<void(int dependent_task_id)>& submit_ready);

  /**
   * @brief Releases dependent tasks and returns ready ids as a batch.
   *
   * @param current_task_id Dense id of the completed PlannedTask.
   * @return Dependent task ids whose counters reached zero.
   * @throws std::out_of_range if current_task_id is invalid.
   * @note Batch release lets dispatchers submit ready handles with one queue
   * operation instead of one lock/notify per dependent tile.
   */
  std::vector<int> release_dependents(int current_task_id);

 private:
  /** @brief Builds the node id to dense execution index lookup. */
  void build_dense_index(const std::vector<int>& execution_order);

  /** @brief Builds counters and dependent adjacency from task dependencies. */
  void build_dependency_state(
      const ComputeTaskGraph& task_graph,
      const std::unordered_set<int>* active_task_ids,
      const std::vector<std::vector<int>>* dependency_task_ids = nullptr);

  /** @brief Node id to dense execution index lookup. */
  std::unordered_map<int, int> id_to_idx_;

  /** @brief Remaining dependency count for each dense planned task. */
  std::vector<std::atomic<int>> dependency_counters_;

  /** @brief Dependent task ids keyed by upstream task id. */
  std::vector<std::vector<int>> dependents_map_;

  /** @brief Active task flags aligned with dependency_counters_. */
  std::vector<bool> active_tasks_;
};

}  // namespace ps::compute
