#include "kernel/services/compute-service/compute_task_planner.hpp"

#include <algorithm>
#include <unordered_set>

#include "graph_model.hpp"

namespace ps::compute {
namespace {

DirtyDomain domain_for_intent(ComputeIntent intent) {
  return intent == ComputeIntent::RealTimeUpdate ? DirtyDomain::RealTime
                                                 : DirtyDomain::HighPrecision;
}

void append_unique(std::vector<int>& values, int value) {
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(value);
  }
}

cv::Rect merge_optional_rect(const cv::Rect& current, const cv::Rect& next) {
  if (next.width <= 0 || next.height <= 0)
    return current;
  if (current.width <= 0 || current.height <= 0)
    return next;
  const int x0 = std::min(current.x, next.x);
  const int y0 = std::min(current.y, next.y);
  const int x1 = std::max(current.x + current.width, next.x + next.width);
  const int y1 = std::max(current.y + current.height, next.y + next.height);
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

bool same_dependency(const PlannedDependency& dependency, int from_node_id,
                     int to_node_id, const std::string& input_kind) {
  return dependency.from_node_id == from_node_id &&
         dependency.to_node_id == to_node_id &&
         dependency.input_kind == input_kind;
}

void add_dependency(std::vector<PlannedDependency>& dependencies,
                    int from_node_id, int to_node_id,
                    const std::string& input_kind, const cv::Rect& from_roi,
                    const cv::Rect& to_roi, DirtyEdgeDirection direction) {
  auto it = std::find_if(dependencies.begin(), dependencies.end(),
                         [&](const PlannedDependency& dependency) {
                           return same_dependency(dependency, from_node_id,
                                                  to_node_id, input_kind);
                         });
  if (it == dependencies.end()) {
    dependencies.push_back(
        {from_node_id, to_node_id, input_kind, from_roi, to_roi, direction});
    return;
  }
  it->from_roi = merge_optional_rect(it->from_roi, from_roi);
  it->to_roi = merge_optional_rect(it->to_roi, to_roi);
  it->direction = direction;
}

std::vector<int> derive_planned_nodes(const ComputeRequest& request,
                                      const std::vector<int>& execution_order,
                                      const DirtyRegionSnapshot* snapshot) {
  if (!snapshot || snapshot->per_node_dirty_rois.empty()) {
    return execution_order;
  }

  std::unordered_set<int> dirty_nodes;
  for (const auto& [node_id, _] : snapshot->per_node_dirty_rois) {
    dirty_nodes.insert(node_id);
  }

  std::vector<int> planned_nodes;
  for (int node_id : execution_order) {
    if (dirty_nodes.count(node_id)) {
      planned_nodes.push_back(node_id);
    }
  }
  if (planned_nodes.empty() &&
      std::find(execution_order.begin(), execution_order.end(),
                request.target_node_id) != execution_order.end()) {
    planned_nodes.push_back(request.target_node_id);
  }
  return planned_nodes;
}

void populate_node_regions(ComputePlan& result,
                           const DirtyRegionSnapshot* snapshot,
                           DirtyDomain domain) {
  std::unordered_map<int, size_t> work_index;
  work_index.reserve(result.planned_work.size());
  for (size_t i = 0; i < result.planned_work.size(); ++i) {
    work_index[result.planned_work[i].node_id] = i;
  }
  if (!snapshot)
    return;

  for (const auto& [node_id, rois] : snapshot->per_node_dirty_rois) {
    auto it = work_index.find(node_id);
    if (it == work_index.end())
      continue;
    PlannedNodeWork& work = result.planned_work[it->second];
    for (const auto& roi : rois) {
      work.dirty_rois.push_back(roi);
      work.represented_hp_roi =
          merge_optional_rect(work.represented_hp_roi, roi);
      if (domain == DirtyDomain::HighPrecision) {
        work.execution_roi = merge_optional_rect(work.execution_roi, roi);
      }
    }
  }

  for (const auto& tile : snapshot->dirty_tiles) {
    if (tile.domain != domain)
      continue;
    auto it = work_index.find(tile.node_id);
    if (it == work_index.end())
      continue;
    PlannedNodeWork& work = result.planned_work[it->second];
    work.execution_roi =
        merge_optional_rect(work.execution_roi, tile.pixel_roi);
  }

  for (const auto& region : snapshot->dirty_monolithic_nodes) {
    if (region.domain != domain)
      continue;
    auto it = work_index.find(region.node_id);
    if (it == work_index.end())
      continue;
    PlannedNodeWork& work = result.planned_work[it->second];
    work.whole_output = work.whole_output || region.whole_output;
    work.execution_roi =
        merge_optional_rect(work.execution_roi, region.pixel_roi);
    if (domain == DirtyDomain::HighPrecision) {
      work.represented_hp_roi =
          merge_optional_rect(work.represented_hp_roi, region.pixel_roi);
    }
  }
}

void populate_dependencies_from_graph(ComputePlan& result,
                                      const GraphModel* graph) {
  if (!graph)
    return;

  std::unordered_set<int> planned_set(result.planned_nodes.begin(),
                                      result.planned_nodes.end());
  for (int node_id : result.planned_nodes) {
    auto node_it = graph->nodes.find(node_id);
    if (node_it == graph->nodes.end())
      continue;
    const Node& node = node_it->second;
    for (const auto& input : node.image_inputs) {
      if (input.from_node_id >= 0 && planned_set.count(input.from_node_id)) {
        add_dependency(result.task_graph.dependencies, input.from_node_id,
                       node_id, "image", cv::Rect(), cv::Rect(),
                       DirtyEdgeDirection::BackwardDemand);
      }
    }
    for (const auto& input : node.parameter_inputs) {
      if (input.from_node_id >= 0 && planned_set.count(input.from_node_id)) {
        add_dependency(result.task_graph.dependencies, input.from_node_id,
                       node_id, "parameter", cv::Rect(), cv::Rect(),
                       DirtyEdgeDirection::BackwardDemand);
      }
    }
  }
}

void populate_dependencies_from_snapshot(ComputePlan& result,
                                         const DirtyRegionSnapshot* snapshot) {
  if (!snapshot)
    return;

  std::unordered_set<int> planned_set(result.planned_nodes.begin(),
                                      result.planned_nodes.end());
  for (const auto& edge : snapshot->edge_mappings) {
    if (!planned_set.count(edge.from_node_id) ||
        !planned_set.count(edge.to_node_id)) {
      continue;
    }
    add_dependency(result.task_graph.dependencies, edge.from_node_id,
                   edge.to_node_id, "image", edge.from_roi, edge.to_roi,
                   edge.direction);
  }
}

void populate_node_dependency_lists(ComputePlan& result) {
  std::unordered_map<int, size_t> work_index;
  work_index.reserve(result.planned_work.size());
  for (size_t i = 0; i < result.planned_work.size(); ++i) {
    work_index[result.planned_work[i].node_id] = i;
  }

  for (const auto& dependency : result.task_graph.dependencies) {
    auto to_it = work_index.find(dependency.to_node_id);
    if (to_it != work_index.end()) {
      append_unique(result.planned_work[to_it->second].dependency_node_ids,
                    dependency.from_node_id);
    }
    auto from_it = work_index.find(dependency.from_node_id);
    if (from_it != work_index.end()) {
      append_unique(result.planned_work[from_it->second].dependent_node_ids,
                    dependency.to_node_id);
    }
  }
}

void add_task(ComputePlan& result, PlannedTask task) {
  task.task_id = static_cast<int>(result.task_graph.tasks.size());
  auto work_it =
      std::find_if(result.planned_work.begin(), result.planned_work.end(),
                   [&](const PlannedNodeWork& work) {
                     return work.node_id == task.node_id;
                   });
  if (work_it != result.planned_work.end()) {
    work_it->task_ids.push_back(task.task_id);
  }
  result.task_graph.tasks.push_back(std::move(task));
}

void populate_tasks(ComputePlan& result, const DirtyRegionSnapshot* snapshot,
                    DirtyDomain domain) {
  if (snapshot) {
    for (const auto& region : snapshot->dirty_monolithic_nodes) {
      if (region.domain != domain)
        continue;
      if (std::find(result.planned_nodes.begin(), result.planned_nodes.end(),
                    region.node_id) == result.planned_nodes.end()) {
        continue;
      }
      add_task(result, PlannedTask{-1,
                                   region.node_id,
                                   PlannedTaskKind::Monolithic,
                                   domain,
                                   region.pixel_roi,
                                   -1,
                                   -1,
                                   0,
                                   region.whole_output,
                                   {}});
    }
    for (const auto& tile : snapshot->dirty_tiles) {
      if (tile.domain != domain)
        continue;
      if (std::find(result.planned_nodes.begin(), result.planned_nodes.end(),
                    tile.node_id) == result.planned_nodes.end()) {
        continue;
      }
      add_task(result, PlannedTask{-1,
                                   tile.node_id,
                                   PlannedTaskKind::Tile,
                                   domain,
                                   tile.pixel_roi,
                                   tile.tile_x,
                                   tile.tile_y,
                                   tile.tile_size,
                                   false,
                                   {}});
    }
  }

  for (const auto& work : result.planned_work) {
    if (!work.task_ids.empty())
      continue;
    add_task(result, PlannedTask{-1,
                                 work.node_id,
                                 PlannedTaskKind::Node,
                                 domain,
                                 work.execution_roi,
                                 -1,
                                 -1,
                                 0,
                                 work.whole_output,
                                 {}});
  }
}

void populate_task_dependencies(ComputePlan& result) {
  std::unordered_map<int, std::vector<int>> task_ids_by_node;
  for (const auto& task : result.task_graph.tasks) {
    task_ids_by_node[task.node_id].push_back(task.task_id);
  }

  std::unordered_set<int> dependent_task_ids;
  for (const auto& dependency : result.task_graph.dependencies) {
    const auto from_it = task_ids_by_node.find(dependency.from_node_id);
    const auto to_it = task_ids_by_node.find(dependency.to_node_id);
    if (from_it == task_ids_by_node.end() || to_it == task_ids_by_node.end())
      continue;
    for (int to_task_id : to_it->second) {
      PlannedTask& to_task = result.task_graph.tasks.at(to_task_id);
      for (int from_task_id : from_it->second) {
        append_unique(to_task.dependency_task_ids, from_task_id);
        dependent_task_ids.insert(to_task_id);
      }
    }
  }

  for (const auto& task : result.task_graph.tasks) {
    if (!dependent_task_ids.count(task.task_id)) {
      result.task_graph.initial_task_ids.push_back(task.task_id);
    }
  }
}

}  // namespace

ComputePlan ComputeTaskPlanner::plan(const ComputeRequest& request,
                                     const std::vector<int>& execution_order,
                                     const DirtyRegionSnapshot* snapshot,
                                     const GraphModel* graph) const {
  ComputePlan result;
  result.intent = request.intent;
  result.target_node_id = request.target_node_id;
  result.parallel = request.parallel;
  result.execution_order = execution_order;
  result.planned_nodes =
      derive_planned_nodes(request, execution_order, snapshot);

  const DirtyDomain domain = domain_for_intent(request.intent);
  result.planned_work.reserve(result.planned_nodes.size());
  for (int node_id : result.planned_nodes) {
    PlannedNodeWork work;
    work.node_id = node_id;
    work.domain = domain;
    result.planned_work.push_back(std::move(work));
  }

  populate_node_regions(result, snapshot, domain);
  populate_dependencies_from_graph(result, graph);
  populate_dependencies_from_snapshot(result, snapshot);
  populate_node_dependency_lists(result);
  populate_tasks(result, snapshot, domain);
  populate_task_dependencies(result);
  return result;
}

}  // namespace ps::compute
