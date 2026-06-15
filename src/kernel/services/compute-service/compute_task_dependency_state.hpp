#pragma once

#include <atomic>
#include <functional>
#include <unordered_map>
#include <vector>

#include "kernel/services/compute-service/task_graph_planning.hpp"

namespace ps::compute {

/**
 * @brief Dense runtime dependency state for a pruned ComputeTaskGraph.
 *
 * TaskDependencyState converts node-id based plan dependencies into compact
 * scheduler counters and dependent adjacency lists. It owns only per-dispatch
 * runtime bookkeeping; the immutable ComputeTaskGraph remains owned by the
 * submission plan.
 *
 * @note The state is not thread-safe for topology mutation. Worker threads may
 * call release_dependents() after tasks are built, while the owning
 * TaskSubmissionPlan keeps the object alive until scheduler completion.
 */
class TaskDependencyState {
 public:
  /**
   * @brief Builds dense dependency counters from planned nodes and task edges.
   *
   * @param execution_order Dense planned node ids produced by the pruned plan.
   * @param task_graph Task graph containing node-level dependency metadata.
   * @throws std::bad_alloc if counter, map, or adjacency storage cannot grow.
   * @note Dependencies whose endpoints are not in execution_order are ignored
   * because they were pruned out of this dispatch.
   */
  TaskDependencyState(const std::vector<int>& execution_order,
                      const ComputeTaskGraph& task_graph);

  /**
   * @brief Returns the node id to dense execution-index map.
   *
   * @return Const reference used by worker input resolution.
   * @throws Nothing.
   * @note The reference remains valid only while this state object lives.
   */
  const std::unordered_map<int, int>& id_to_idx() const { return id_to_idx_; }

  /**
   * @brief Checks whether a dense node can be submitted as initial work.
   *
   * @param node_idx Dense execution index being considered.
   * @return true when the node has no remaining dependencies.
   * @throws std::out_of_range if node_idx is not a valid dense index.
   * @note Uses acquire ordering so initial submission observes dependency
   * counter initialization.
   */
  bool ready_for_initial_submit(int node_idx) const;

  /**
   * @brief Releases dependent nodes after one upstream node completes.
   *
   * @param current_node_idx Dense execution index of the completed node.
   * @param submit_ready Callback invoked with each dependent dense index whose
   * counter reaches zero.
   * @throws std::out_of_range for inconsistent dense indexes, or exceptions
   * propagated by submit_ready.
   * @note The caller is responsible for publishing produced outputs before
   * invoking this method.
   */
  void release_dependents(
      int current_node_idx,
      const std::function<void(int dependent_idx)>& submit_ready);

 private:
  /** @brief Builds the node id to dense execution index lookup. */
  void build_dense_index(const std::vector<int>& execution_order);

  /** @brief Builds counters and dependent adjacency from task dependencies. */
  void build_dependency_state(const std::vector<int>& execution_order,
                              const ComputeTaskGraph& task_graph);

  /** @brief Node id to dense execution index lookup. */
  std::unordered_map<int, int> id_to_idx_;

  /** @brief Remaining dependency count for each dense planned node. */
  std::vector<std::atomic<int>> dependency_counters_;

  /** @brief Dense dependent node indexes keyed by upstream dense index. */
  std::vector<std::vector<int>> dependents_map_;
};

}  // namespace ps::compute
