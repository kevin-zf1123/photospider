#include "kernel/services/compute-service/compute_task_dependency_state.hpp"

#include <unordered_set>
#include <vector>

namespace ps::compute {

TaskDependencyState::TaskDependencyState(
    const std::vector<int>& execution_order,
    const ComputeTaskGraph& task_graph) {
  build_dense_index(execution_order);
  build_dependency_state(execution_order, task_graph);
}

bool TaskDependencyState::ready_for_initial_submit(int node_idx) const {
  return dependency_counters_.at(node_idx).load(std::memory_order_acquire) == 0;
}

void TaskDependencyState::release_dependents(
    int current_node_idx,
    const std::function<void(int dependent_idx)>& submit_ready) {
  for (int dependent_idx : dependents_map_.at(current_node_idx)) {
    const int previous = dependency_counters_.at(dependent_idx)
                             .fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 1) {
      submit_ready(dependent_idx);
    }
  }
}

void TaskDependencyState::build_dense_index(
    const std::vector<int>& execution_order) {
  id_to_idx_.reserve(execution_order.size());
  for (size_t i = 0; i < execution_order.size(); ++i) {
    id_to_idx_[execution_order[i]] = static_cast<int>(i);
  }
}

void TaskDependencyState::build_dependency_state(
    const std::vector<int>& execution_order,
    const ComputeTaskGraph& task_graph) {
  const size_t node_count = execution_order.size();
  dependency_counters_ = std::vector<std::atomic<int>>(node_count);
  dependents_map_.assign(node_count, {});
  for (auto& counter : dependency_counters_) {
    counter.store(0, std::memory_order_relaxed);
  }

  std::unordered_set<int> execution_set(execution_order.begin(),
                                        execution_order.end());
  for (const auto& dependency : task_graph.dependencies) {
    if (!execution_set.count(dependency.from_node_id) ||
        !execution_set.count(dependency.to_node_id)) {
      continue;
    }
    const int dep_idx = id_to_idx_.at(dependency.from_node_id);
    const int dependent_idx = id_to_idx_.at(dependency.to_node_id);
    dependents_map_[dep_idx].push_back(dependent_idx);
    dependency_counters_[dependent_idx].fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace ps::compute
