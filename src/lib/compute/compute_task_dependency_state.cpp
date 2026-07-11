#include "compute/compute_task_dependency_state.hpp"

#include <unordered_set>
#include <vector>

namespace ps::compute {

TaskDependencyState::TaskDependencyState(
    const std::vector<int>& execution_order,
    const ComputeTaskGraph& task_graph) {
  build_dense_index(execution_order);
  build_dependency_state(task_graph, nullptr, nullptr);
}

TaskDependencyState::TaskDependencyState(
    const std::vector<int>& execution_order, const ComputeTaskGraph& task_graph,
    const std::vector<int>& active_task_ids) {
  build_dense_index(execution_order);
  std::unordered_set<int> active(active_task_ids.begin(),
                                 active_task_ids.end());
  build_dependency_state(task_graph, &active, nullptr);
}

TaskDependencyState::TaskDependencyState(
    const std::vector<int>& execution_order, const ComputeTaskGraph& task_graph,
    const std::vector<int>& active_task_ids,
    const std::vector<std::vector<int>>& dependency_task_ids) {
  build_dense_index(execution_order);
  std::unordered_set<int> active(active_task_ids.begin(),
                                 active_task_ids.end());
  build_dependency_state(task_graph, &active, &dependency_task_ids);
}

bool TaskDependencyState::ready_for_initial_submit(int task_id) const {
  return active_tasks_.at(task_id) &&
         dependency_counters_.at(task_id).load(std::memory_order_acquire) == 0;
}

void TaskDependencyState::release_dependents(
    int current_task_id,
    const std::function<void(int dependent_task_id)>& submit_ready) {
  for (int dependent_task_id : release_dependents(current_task_id)) {
    submit_ready(dependent_task_id);
  }
}

std::vector<int> TaskDependencyState::release_dependents(int current_task_id) {
  std::vector<int> ready;
  for (int dependent_task_id : dependents_map_.at(current_task_id)) {
    const int previous = dependency_counters_.at(dependent_task_id)
                             .fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 1) {
      ready.push_back(dependent_task_id);
    }
  }
  return ready;
}

void TaskDependencyState::build_dense_index(
    const std::vector<int>& execution_order) {
  id_to_idx_.reserve(execution_order.size());
  for (size_t i = 0; i < execution_order.size(); ++i) {
    id_to_idx_[execution_order[i]] = static_cast<int>(i);
  }
}

void TaskDependencyState::build_dependency_state(
    const ComputeTaskGraph& task_graph,
    const std::unordered_set<int>* active_task_ids,
    const std::vector<std::vector<int>>* dependency_task_ids) {
  const size_t task_count = task_graph.tasks.size();
  dependency_counters_ = std::vector<std::atomic<int>>(task_count);
  dependents_map_.assign(task_count, {});
  active_tasks_.assign(task_count, false);
  for (auto& counter : dependency_counters_) {
    counter.store(0, std::memory_order_relaxed);
  }

  auto is_active = [&](int task_id) {
    if (task_id < 0 || task_id >= static_cast<int>(task_count)) {
      return false;
    }
    return active_task_ids == nullptr || active_task_ids->count(task_id) > 0;
  };

  for (const auto& task : task_graph.tasks) {
    if (task.task_id < 0 || task.task_id >= static_cast<int>(task_count)) {
      continue;
    }
    active_tasks_[task.task_id] = is_active(task.task_id);
  }

  for (const auto& task : task_graph.tasks) {
    if (!is_active(task.task_id)) {
      continue;
    }
    const std::vector<int>& dependencies =
        dependency_task_ids ? dependency_task_ids->at(task.task_id)
                            : task.dependency_task_ids;
    for (int dependency_task_id : dependencies) {
      if (!is_active(dependency_task_id)) {
        continue;
      }
      dependents_map_.at(dependency_task_id).push_back(task.task_id);
      dependency_counters_.at(task.task_id)
          .fetch_add(1, std::memory_order_relaxed);
    }
  }
}

}  // namespace ps::compute
