#include "kernel/services/compute-service/task_graph_planning.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "graph_model.hpp"
#include "kernel/services/compute-service/compute_cache_policy.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"
#include "kernel/services/graph_extent_resolver.hpp"

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
                    int from_node_id, int to_node_id, DirtyDomain domain,
                    const std::string& input_kind, const cv::Rect& from_roi,
                    const cv::Rect& to_roi, DirtyEdgeDirection direction) {
  auto it = std::find_if(dependencies.begin(), dependencies.end(),
                         [&](const PlannedDependency& dependency) {
                           return same_dependency(dependency, from_node_id,
                                                  to_node_id, input_kind) &&
                                  dependency.domain == domain;
                         });
  if (it == dependencies.end()) {
    dependencies.push_back({from_node_id, to_node_id, domain, input_kind,
                            from_roi, to_roi, direction});
    return;
  }
  it->from_roi = merge_optional_rect(it->from_roi, from_roi);
  it->to_roi = merge_optional_rect(it->to_roi, to_roi);
  it->direction = direction;
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

void populate_dependencies_from_graph(ComputePlan& result, DirtyDomain domain,
                                      const GraphModel* graph) {
  if (!graph)
    return;

  std::unordered_set<int> planned_set(result.planned_nodes.begin(),
                                      result.planned_nodes.end());
  for (int node_id : result.planned_nodes) {
    if (!graph->has_node(node_id))
      continue;
    for (const auto& edge : graph->upstream_edges(node_id)) {
      if (edge.from_node_id < 0 || !planned_set.count(edge.from_node_id)) {
        continue;
      }
      const char* kind = edge.kind == GraphTopologyEdgeKind::ImageInput
                             ? "image"
                             : "parameter";
      add_dependency(result.task_graph.dependencies, edge.from_node_id, node_id,
                     domain, kind, cv::Rect(), cv::Rect(),
                     DirtyEdgeDirection::BackwardDemand);
    }
  }
}

void populate_dependencies_from_snapshot(ComputePlan& result,
                                         const DirtyRegionSnapshot* snapshot,
                                         DirtyDomain domain) {
  if (!snapshot)
    return;

  std::unordered_set<int> planned_set(result.planned_nodes.begin(),
                                      result.planned_nodes.end());
  for (const auto& edge : snapshot->edge_mappings) {
    if (edge.domain != domain)
      continue;
    if (!planned_set.count(edge.from_node_id) ||
        !planned_set.count(edge.to_node_id)) {
      continue;
    }
    add_dependency(result.task_graph.dependencies, edge.from_node_id,
                   edge.to_node_id, domain, "image", edge.from_roi, edge.to_roi,
                   edge.direction);
  }
}

void populate_node_dependency_lists(ComputePlan& result) {
  std::unordered_map<int, size_t> work_index;
  work_index.reserve(result.planned_work.size());
  for (size_t i = 0; i < result.planned_work.size(); ++i) {
    result.planned_work[i].dependency_node_ids.clear();
    result.planned_work[i].dependent_node_ids.clear();
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

bool is_dirty_source(const DirtyRegionSnapshot* snapshot, int node_id) {
  if (!snapshot)
    return false;
  return std::find(snapshot->dirty_source_nodes.begin(),
                   snapshot->dirty_source_nodes.end(),
                   node_id) != snapshot->dirty_source_nodes.end();
}

cv::Rect dirty_roi_for_node(const DirtyRegionSnapshot* snapshot, int node_id,
                            DirtyDomain domain) {
  if (!snapshot)
    return cv::Rect();
  cv::Rect merged;
  auto actual_it = snapshot->actual_dirty_rois.find(node_id);
  if (actual_it != snapshot->actual_dirty_rois.end()) {
    for (const auto& roi : actual_it->second) {
      merged = merge_optional_rect(merged, roi);
    }
  }
  auto roi_it = snapshot->per_node_dirty_rois.find(node_id);
  if (roi_it != snapshot->per_node_dirty_rois.end()) {
    for (const auto& roi : roi_it->second) {
      merged = merge_optional_rect(merged, roi);
    }
  }
  for (const auto& tile : snapshot->dirty_tiles) {
    if (tile.domain == domain && tile.node_id == node_id) {
      merged = merge_optional_rect(merged, tile.pixel_roi);
    }
  }
  for (const auto& region : snapshot->dirty_monolithic_nodes) {
    if (region.domain == domain && region.node_id == node_id) {
      merged = merge_optional_rect(merged, region.pixel_roi);
    }
  }
  return merged;
}

bool intersects_dirty_roi(const PlannedTask& task,
                          const DirtyRegionSnapshot* snapshot) {
  if (!snapshot)
    return true;
  const cv::Rect dirty_roi =
      dirty_roi_for_node(snapshot, task.node_id, task.domain);
  if (dirty_roi.width <= 0 || dirty_roi.height <= 0)
    return false;
  if (task.output_roi.width <= 0 || task.output_roi.height <= 0)
    return true;
  return (task.output_roi & dirty_roi).area() > 0;
}

int tile_size_for_node(const Node& node) {
  int tile_size = 128;
  auto meta = OpRegistry::instance().get_metadata(node.type, node.subtype);
  if (!meta)
    return tile_size;
  if (meta->tile_preference == TileSizePreference::MICRO)
    return 16;
  if (meta->tile_preference == TileSizePreference::MACRO)
    return 256;
  return tile_size;
}

bool has_tiled_impl(const Node& node, DirtyDomain domain) {
  const auto* impls =
      OpRegistry::instance().get_implementations(node.type, node.subtype);
  if (!impls)
    return false;
  return domain == DirtyDomain::RealTime ? static_cast<bool>(impls->tiled_rt)
                                         : static_cast<bool>(impls->tiled_hp);
}

bool has_monolithic_impl(const Node& node, DirtyDomain domain) {
  const auto* impls =
      OpRegistry::instance().get_implementations(node.type, node.subtype);
  if (!impls)
    return false;
  (void)domain;
  return static_cast<bool>(impls->monolithic_hp);
}

void apply_task_dirty_metadata(PlannedTask& task,
                               const DirtyRegionSnapshot* snapshot) {
  task.source_boundary_eligible = is_dirty_source(snapshot, task.node_id);
  task.dirty_generation = snapshot ? snapshot->graph_generation : 0;
  task.dirty_selected = intersects_dirty_roi(task, snapshot);
}

void add_task(ComputePlan& result, PlannedTask task,
              const DirtyRegionSnapshot* snapshot) {
  task.task_id = static_cast<int>(result.task_graph.tasks.size());
  apply_task_dirty_metadata(task, snapshot);
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
                    DirtyDomain domain, const GraphModel* graph) {
  if (!graph && snapshot) {
    for (const auto& region : snapshot->dirty_monolithic_nodes) {
      if (region.domain != domain)
        continue;
      if (std::find(result.planned_nodes.begin(), result.planned_nodes.end(),
                    region.node_id) == result.planned_nodes.end()) {
        continue;
      }
      add_task(result,
               PlannedTask{-1,
                           region.node_id,
                           PlannedTaskKind::Monolithic,
                           domain,
                           region.pixel_roi,
                           -1,
                           -1,
                           0,
                           region.whole_output,
                           false,
                           false,
                           0,
                           {}},
               snapshot);
    }
    for (const auto& tile : snapshot->dirty_tiles) {
      if (tile.domain != domain)
        continue;
      if (std::find(result.planned_nodes.begin(), result.planned_nodes.end(),
                    tile.node_id) == result.planned_nodes.end()) {
        continue;
      }
      add_task(result,
               PlannedTask{-1,
                           tile.node_id,
                           PlannedTaskKind::Tile,
                           domain,
                           tile.pixel_roi,
                           tile.tile_x,
                           tile.tile_y,
                           tile.tile_size,
                           false,
                           false,
                           false,
                           0,
                           {}},
               snapshot);
    }
    for (const auto& work : result.planned_work) {
      if (!work.task_ids.empty())
        continue;
      add_task(result,
               PlannedTask{-1,
                           work.node_id,
                           PlannedTaskKind::Node,
                           domain,
                           work.execution_roi,
                           -1,
                           -1,
                           0,
                           work.whole_output,
                           false,
                           false,
                           0,
                           {}},
               snapshot);
    }
    return;
  }
  if (!graph) {
    for (const auto& work : result.planned_work) {
      add_task(result,
               PlannedTask{-1,
                           work.node_id,
                           PlannedTaskKind::Node,
                           domain,
                           work.execution_roi,
                           -1,
                           -1,
                           0,
                           work.whole_output,
                           false,
                           false,
                           0,
                           {}},
               snapshot);
    }
    return;
  }

  GraphExtentResolver extent_resolver;
  std::unordered_map<int, cv::Size> extent_cache;
  for (const auto& work : result.planned_work) {
    if (!graph->has_node(work.node_id))
      continue;
    const Node& node = graph->node(work.node_id);
    cv::Size extent = extent_resolver.resolve_output_extent(
        const_cast<GraphModel&>(*graph), work.node_id, extent_cache);
    cv::Rect full_output(0, 0, std::max(0, extent.width),
                         std::max(0, extent.height));
    if (has_tiled_impl(node, domain) && full_output.width > 0 &&
        full_output.height > 0) {
      const int tile_size = tile_size_for_node(node);
      for (int y = 0; y < full_output.height; y += tile_size) {
        for (int x = 0; x < full_output.width; x += tile_size) {
          cv::Rect tile_roi(x, y,
                            std::min(tile_size, full_output.width - x),
                            std::min(tile_size, full_output.height - y));
          add_task(result,
                   PlannedTask{-1,
                               work.node_id,
                               PlannedTaskKind::Tile,
                               domain,
                               tile_roi,
                               x / tile_size,
                               y / tile_size,
                               tile_size,
                               false,
                               false,
                               false,
                               0,
                               {}},
                   snapshot);
        }
      }
      continue;
    }

    const PlannedTaskKind kind =
        has_monolithic_impl(node, domain) ? PlannedTaskKind::Monolithic
                                         : PlannedTaskKind::Node;
    add_task(result,
             PlannedTask{-1,
                         work.node_id,
                         kind,
                         domain,
                         full_output.width > 0 ? full_output
                                               : work.execution_roi,
                         -1,
                         -1,
                         0,
                         kind == PlannedTaskKind::Monolithic,
                         false,
                         false,
                         0,
                         {}},
             snapshot);
  }
}

void populate_task_dependencies(ComputePlan& result) {
  result.task_graph.initial_task_ids.clear();
  for (auto& task : result.task_graph.tasks) {
    task.dependency_task_ids.clear();
  }

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

void rebuild_work_task_ids(ComputePlan& result) {
  std::unordered_map<int, size_t> work_index;
  work_index.reserve(result.planned_work.size());
  for (size_t i = 0; i < result.planned_work.size(); ++i) {
    result.planned_work[i].task_ids.clear();
    work_index[result.planned_work[i].node_id] = i;
  }
  for (const auto& task : result.task_graph.tasks) {
    auto work_it = work_index.find(task.node_id);
    if (work_it != work_index.end()) {
      result.planned_work[work_it->second].task_ids.push_back(task.task_id);
    }
  }
}

void clear_dirty_work_metadata(ComputePlan& result) {
  for (auto& work : result.planned_work) {
    work.represented_hp_roi = cv::Rect();
    work.execution_roi = cv::Rect();
    work.whole_output = false;
    work.dirty_rois.clear();
  }
  for (auto& task : result.task_graph.tasks) {
    task.source_boundary_eligible = false;
    task.dirty_selected = true;
    task.dirty_generation = 0;
  }
}

ComputePlan build_plan_from_nodes(const ComputeRequest& request,
                                  const std::vector<int>& planned_nodes,
                                  const DirtyRegionSnapshot* snapshot,
                                  const GraphModel* graph) {
  ComputePlan result;
  result.intent = request.intent;
  result.target_node_id = request.target_node_id;
  result.parallel = request.parallel;
  result.execution_order = planned_nodes;
  result.planned_nodes = planned_nodes;

  const DirtyDomain domain = domain_for_intent(request.intent);
  result.planned_work.reserve(result.planned_nodes.size());
  for (int node_id : result.planned_nodes) {
    PlannedNodeWork work;
    work.node_id = node_id;
    work.domain = domain;
    result.planned_work.push_back(std::move(work));
  }

  populate_node_regions(result, snapshot, domain);
  populate_dependencies_from_graph(result, domain, graph);
  populate_dependencies_from_snapshot(result, snapshot, domain);
  populate_node_dependency_lists(result);
  populate_tasks(result, snapshot, domain, graph);
  populate_task_dependencies(result);
  return result;
}

DirtyUpdateWorkSet collect_dirty_source_tasks(
    const ComputePlan& plan, const DirtyRegionSnapshot& snapshot) {
  DirtyUpdateWorkSet work_set;
  work_set.generation = snapshot.graph_generation;
  std::unordered_set<int> source_nodes(snapshot.dirty_source_nodes.begin(),
                                       snapshot.dirty_source_nodes.end());
  for (const auto& task : plan.task_graph.tasks) {
    if (source_nodes.count(task.node_id)) {
      work_set.dirty_source_task_ids.push_back(task.task_id);
    }
  }
  return work_set;
}

}  // namespace

FullTaskGraph FullTaskGraphExpander::expand(const GraphModel& graph,
                                            ComputeIntent intent) const {
  ComputeRequest request;
  request.intent = intent;
  const ComputePlan full_plan =
      build_plan_from_nodes(request, graph.node_ids(), nullptr, &graph);

  FullTaskGraph expanded;
  expanded.intent = intent;
  expanded.domain = domain_for_intent(intent);
  expanded.expanded_node_ids = full_plan.planned_nodes;
  expanded.expanded_work = full_plan.planned_work;
  expanded.task_graph = full_plan.task_graph;
  return expanded;
}

ComputePlan NodeCacheTaskGraphPruner::prune(
    const FullTaskGraph& full_graph, const ComputeRequest& request,
    const std::vector<int>& execution_order, const GraphModel& graph) const {
  ComputePlan result;
  result.intent = request.intent;
  result.target_node_id = request.target_node_id;
  result.parallel = request.parallel;
  result.execution_order = execution_order;
  result.planned_nodes = execution_order;

  std::unordered_set<int> selected_nodes(execution_order.begin(),
                                         execution_order.end());
  std::unordered_map<int, const PlannedNodeWork*> full_work_by_node;
  full_work_by_node.reserve(full_graph.expanded_work.size());
  for (const auto& work : full_graph.expanded_work) {
    full_work_by_node[work.node_id] = &work;
  }

  result.planned_work.reserve(execution_order.size());
  for (int node_id : execution_order) {
    if (!graph.has_node(node_id)) {
      throw GraphError(GraphErrc::NotFound,
                       "Cannot prune task graph: node " +
                           std::to_string(node_id) + " not found.");
    }
    auto full_work_it = full_work_by_node.find(node_id);
    if (full_work_it == full_work_by_node.end()) {
      throw GraphError(GraphErrc::ComputeError,
                       "Full task graph is missing node " +
                           std::to_string(node_id) + ".");
    }
    PlannedNodeWork work = *full_work_it->second;
    work.task_ids.clear();
    work.dependency_node_ids.clear();
    work.dependent_node_ids.clear();
    work.dirty_rois.clear();
    work.reusable_cache_available =
        ComputeCachePolicy::has_reusable_output(graph.node(node_id));
    result.planned_work.push_back(std::move(work));
  }

  for (const auto& task : full_graph.task_graph.tasks) {
    if (!selected_nodes.count(task.node_id))
      continue;
    PlannedTask pruned_task = task;
    pruned_task.task_id = static_cast<int>(result.task_graph.tasks.size());
    pruned_task.dependency_task_ids.clear();
    result.task_graph.tasks.push_back(std::move(pruned_task));
  }

  for (const auto& dependency : full_graph.task_graph.dependencies) {
    if (!selected_nodes.count(dependency.from_node_id) ||
        !selected_nodes.count(dependency.to_node_id)) {
      continue;
    }
    result.task_graph.dependencies.push_back(dependency);
  }

  rebuild_work_task_ids(result);
  populate_node_dependency_lists(result);
  populate_task_dependencies(result);
  return result;
}

ComputePlan DirtySnapshotTaskGraphPruner::prune(
    const ComputePlan& node_cache_plan,
    const DirtyRegionSnapshot& snapshot) const {
  ComputePlan result = node_cache_plan;
  const DirtyDomain domain = domain_for_intent(result.intent);

  clear_dirty_work_metadata(result);
  populate_node_regions(result, &snapshot, domain);
  populate_dependencies_from_snapshot(result, &snapshot, domain);
  for (auto& task : result.task_graph.tasks) {
    apply_task_dirty_metadata(task, &snapshot);
  }
  rebuild_work_task_ids(result);
  populate_node_dependency_lists(result);
  populate_task_dependencies(result);
  return result;
}

DirtyUpdateWorkSet DirtySnapshotTaskGraphPruner::materialize(
    const ComputePlan& plan, const DirtyRegionSnapshot& snapshot) const {
  DirtyUpdateWorkSet work_set = collect_dirty_source_tasks(plan, snapshot);
  std::unordered_set<int> source_nodes(snapshot.dirty_source_nodes.begin(),
                                       snapshot.dirty_source_nodes.end());
  for (const auto& task : plan.task_graph.tasks) {
    if (source_nodes.count(task.node_id))
      continue;
    if (task.dirty_selected) {
      work_set.downstream_task_ids.push_back(task.task_id);
    }
  }
  return work_set;
}

std::vector<int> TaskGraphReadyChecker::initial_ready_task_ids(
    const ComputeTaskGraph& graph,
    const std::vector<int>* allowed_task_ids) const {
  std::unordered_set<int> allowed;
  if (allowed_task_ids) {
    allowed.insert(allowed_task_ids->begin(), allowed_task_ids->end());
  }
  std::vector<int> ready;
  for (const auto& task : graph.tasks) {
    if (allowed_task_ids && !allowed.count(task.task_id))
      continue;
    bool all_dependencies_allowed = true;
    for (int dependency_id : task.dependency_task_ids) {
      if (!allowed_task_ids || allowed.count(dependency_id)) {
        all_dependencies_allowed = false;
        break;
      }
    }
    if (all_dependencies_allowed) {
      ready.push_back(task.task_id);
    }
  }
  return ready;
}

}  // namespace ps::compute
