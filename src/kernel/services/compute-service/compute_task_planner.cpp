#include "kernel/services/compute-service/compute_task_planner.hpp"

#include <algorithm>
#include <unordered_set>

namespace ps::compute {

ComputePlan ComputeTaskPlanner::plan(
    const ComputeRequest& request, const std::vector<int>& execution_order,
    const DirtyRegionSnapshot* snapshot) const {
  ComputePlan result;
  result.intent = request.intent;
  result.target_node_id = request.target_node_id;
  result.parallel = request.parallel;
  result.execution_order = execution_order;

  if (!snapshot || snapshot->per_node_dirty_rois.empty()) {
    result.planned_nodes = execution_order;
    return result;
  }

  std::unordered_set<int> dirty_nodes;
  for (const auto& [node_id, _] : snapshot->per_node_dirty_rois) {
    dirty_nodes.insert(node_id);
  }
  for (int node_id : execution_order) {
    if (dirty_nodes.count(node_id))
      result.planned_nodes.push_back(node_id);
  }
  if (result.planned_nodes.empty() &&
      std::find(execution_order.begin(), execution_order.end(),
                request.target_node_id) != execution_order.end()) {
    result.planned_nodes.push_back(request.target_node_id);
  }
  return result;
}

}  // namespace ps::compute
