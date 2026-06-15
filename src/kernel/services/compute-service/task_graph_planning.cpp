#include "kernel/services/compute-service/task_graph_planning.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "graph_model.hpp"  // NOLINT(build/include_subdir)
#include "kernel/services/compute-service/compute_cache_policy.hpp"
#include "kernel/services/compute-service/task_population_strategy.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Maps a compute intent to the single dirty/task domain being planned.
 *
 * @param intent Compute intent supplied by the request.
 * @return RealTime for realtime intent, otherwise HighPrecision.
 * @throws Nothing.
 * @note Callers must invoke this per HP or RT path; the function never creates
 * a mixed-domain plan.
 */
DirtyDomain domain_for_intent(ComputeIntent intent) {
  return intent == ComputeIntent::RealTimeUpdate ? DirtyDomain::RealTime
                                                 : DirtyDomain::HighPrecision;
}

/**
 * @brief Appends a value only when the vector does not already contain it.
 *
 * @param values Vector being updated in stable insertion order.
 * @param value Node or task id to append.
 * @throws std::bad_alloc if vector growth fails.
 * @note The helper preserves first-seen order for inspection output.
 */
void append_unique(std::vector<int>& values, int value) {
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(value);
  }
}

/**
 * @brief Merges two optional cv::Rect values using empty-as-missing semantics.
 *
 * @param current Current accumulated rectangle, possibly empty.
 * @param next Next rectangle to merge, possibly empty.
 * @return Union rectangle, or the non-empty side when only one side is valid.
 * @throws Nothing.
 * @note Empty rectangles are used as "unknown/no ROI" rather than geometry.
 */
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

/**
 * @brief Checks whether an existing dependency matches node endpoints and kind.
 *
 * @param dependency Existing dependency entry.
 * @param from_node_id Candidate upstream node id.
 * @param to_node_id Candidate downstream node id.
 * @param input_kind Candidate input kind label.
 * @return true when endpoints and input kind match.
 * @throws Nothing.
 * @note Domain is checked by add_dependency() because same endpoints can exist
 * in HP and RT plans independently.
 */
bool same_dependency(const PlannedDependency& dependency, int from_node_id,
                     int to_node_id, const std::string& input_kind) {
  return dependency.from_node_id == from_node_id &&
         dependency.to_node_id == to_node_id &&
         dependency.input_kind == input_kind;
}

/**
 * @brief Adds or merges one planned dependency into a dependency vector.
 *
 * @param dependencies Mutable dependency list owned by ComputeTaskGraph.
 * @param next New dependency edge to append or merge by endpoint/kind/domain.
 * @throws std::bad_alloc if dependency storage grows.
 * @note Duplicate dependencies merge ROI metadata so snapshot and graph edges
 * preserve one edge record per endpoint/kind/domain.
 */
void add_dependency(std::vector<PlannedDependency>& dependencies,
                    PlannedDependency next) {
  auto it =
      std::find_if(dependencies.begin(), dependencies.end(),
                   [&](const PlannedDependency& dependency) {
                     return same_dependency(dependency, next.from_node_id,
                                            next.to_node_id, next.input_kind) &&
                            dependency.domain == next.domain;
                   });
  if (it == dependencies.end()) {
    dependencies.push_back(std::move(next));
    return;
  }
  it->from_roi = merge_optional_rect(it->from_roi, next.from_roi);
  it->to_roi = merge_optional_rect(it->to_roi, next.to_roi);
  it->direction = next.direction;
}

/**
 * @brief Copies dirty ROI metadata from a snapshot onto planned node work.
 *
 * @param result Plan whose PlannedNodeWork records are updated.
 * @param snapshot Optional dirty snapshot; null leaves node work unchanged.
 * @param domain HP or RT domain being planned.
 * @throws std::bad_alloc if temporary lookup storage grows.
 * @note High-precision plans also update represented_hp_roi because HP output
 * remains the authoritative ROI space.
 */
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

/**
 * @brief Adds graph topology dependencies between planned nodes.
 *
 * @param result Plan whose task_graph.dependencies receives graph edges.
 * @param domain HP or RT domain for the dependency records.
 * @param graph Optional source graph; null skips graph dependency discovery.
 * @throws std::bad_alloc if dependency or temporary set storage grows.
 * @note Only dependencies whose upstream node survived pruning are recorded.
 */
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
      add_dependency(result.task_graph.dependencies,
                     PlannedDependency{edge.from_node_id, node_id, domain, kind,
                                       cv::Rect(), cv::Rect(),
                                       DirtyEdgeDirection::BackwardDemand});
    }
  }
}

/**
 * @brief Adds dirty snapshot edge mappings between planned nodes.
 *
 * @param result Plan whose task_graph.dependencies receives dirty mappings.
 * @param snapshot Optional dirty snapshot; null skips this source.
 * @param domain HP or RT domain for the dependency records.
 * @throws std::bad_alloc if dependency or temporary set storage grows.
 * @note Edge mappings outside the selected plan or different domain are
 * ignored so HP and RT paths remain independent.
 */
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
    add_dependency(
        result.task_graph.dependencies,
        PlannedDependency{edge.from_node_id, edge.to_node_id, domain, "image",
                          edge.from_roi, edge.to_roi, edge.direction});
  }
}

/**
 * @brief Rebuilds per-node dependency and dependent node lists from edges.
 *
 * @param result Plan whose PlannedNodeWork dependency lists are refreshed.
 * @throws std::bad_alloc if lookup or node-list storage grows.
 * @note The lists are diagnostic and planning metadata; task-level dependency
 * ids are rebuilt separately after task population.
 */
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

/**
 * @brief Builds task-level dependency ids and initial ready task ids.
 *
 * @param result Plan whose ComputeTaskGraph task dependency metadata is
 * refreshed.
 * @throws std::bad_alloc if node-to-task lookup or ready ids grow.
 * @note Every task belonging to an upstream node is treated as a dependency of
 * every task belonging to the downstream node, matching existing node-level
 * execution semantics.
 */
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

/**
 * @brief Rebuilds PlannedNodeWork::task_ids from the task graph.
 *
 * @param result Plan whose per-node task lists are refreshed.
 * @throws std::bad_alloc if lookup or task-id vectors grow.
 * @note Used after pruning copies tasks because copied task ids may no longer
 * align with the destination task graph.
 */
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

/**
 * @brief Clears dirty ROI metadata before applying a new dirty snapshot.
 *
 * @param result Plan copy being prepared for dirty snapshot pruning.
 * @throws Nothing directly.
 * @note Task selected flags are reset to true so apply_task_dirty_metadata()
 * can re-evaluate them from the new snapshot.
 */
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

/**
 * @brief Builds a ComputePlan from an explicit planned node list.
 *
 * @param request Planning request containing intent, target, and mode flags.
 * @param planned_nodes Node ids to include in execution order.
 * @param snapshot Optional dirty snapshot used for ROI metadata and task
 * population.
 * @param graph Optional graph used for topology dependencies and task shape.
 * @return ComputePlan with node work, dependencies, tasks, and initial ids.
 * @throws GraphError or standard exceptions from task population, graph access,
 * op metadata lookup, or allocation.
 * @note This helper is the shared spine for full graph expansion, node/cache
 * pruning compatibility, and dirty snapshot pruning.
 */
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
  TaskPopulationStrategy task_population;
  task_population.populate(result, snapshot, domain, graph);
  populate_task_dependencies(result);
  return result;
}

/**
 * @brief Collects task ids that belong to dirty source nodes.
 *
 * @param plan Dirty-annotated plan whose task graph is scanned.
 * @param snapshot Dirty snapshot providing generation and source node ids.
 * @return Work set with generation and source task ids populated.
 * @throws std::bad_alloc if temporary sets or output vectors grow.
 * @note Downstream dirty task ids are appended later by materialize().
 */
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
      throw GraphError(GraphErrc::NotFound, "Cannot prune task graph: node " +
                                                std::to_string(node_id) +
                                                " not found.");
    }
    auto full_work_it = full_work_by_node.find(node_id);
    if (full_work_it == full_work_by_node.end()) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Full task graph is missing node " + std::to_string(node_id) + ".");
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
