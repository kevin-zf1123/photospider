#include "kernel/services/compute-service/task_population_strategy.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "graph_model.hpp"  // NOLINT(build/include_subdir)
#include "kernel/services/graph_extent_resolver.hpp"

namespace ps::compute {
namespace {

cv::Rect merge_optional_rect(const cv::Rect& current, const cv::Rect& next) {
  if (next.width <= 0 || next.height <= 0) {
    return current;
  }
  if (current.width <= 0 || current.height <= 0) {
    return next;
  }
  const int x0 = std::min(current.x, next.x);
  const int y0 = std::min(current.y, next.y);
  const int x1 = std::max(current.x + current.width, next.x + next.width);
  const int y1 = std::max(current.y + current.height, next.y + next.height);
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

bool is_dirty_source(const DirtyRegionSnapshot* snapshot, int node_id) {
  if (!snapshot) {
    return false;
  }
  return std::find(snapshot->dirty_source_nodes.begin(),
                   snapshot->dirty_source_nodes.end(),
                   node_id) != snapshot->dirty_source_nodes.end();
}

cv::Rect dirty_roi_for_node(const DirtyRegionSnapshot* snapshot, int node_id,
                            DirtyDomain domain) {
  if (!snapshot) {
    return cv::Rect();
  }
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
  if (!snapshot) {
    return true;
  }
  const cv::Rect dirty_roi =
      dirty_roi_for_node(snapshot, task.node_id, task.domain);
  if (dirty_roi.width <= 0 || dirty_roi.height <= 0) {
    return false;
  }
  if (task.output_roi.width <= 0 || task.output_roi.height <= 0) {
    return true;
  }
  return (task.output_roi & dirty_roi).area() > 0;
}

/**
 * @brief Encapsulates HP/RT implementation shape checks for graph-backed tasks.
 *
 * @note Current monolithic lookup still follows the existing HP monolithic
 * field for both domains because the operation registry has no separate RT
 * monolithic slot. Tiled lookup remains domain-specific.
 */
class DomainTaskShapeStrategy {
 public:
  /** @brief Binds task shape decisions to one compute domain. */
  explicit DomainTaskShapeStrategy(DirtyDomain domain) : domain_(domain) {}

  /** @brief Returns true when node has a tiled implementation for the domain.
   */
  bool has_tiled_impl(const Node& node) const {
    const auto* impls =
        OpRegistry::instance().get_implementations(node.type, node.subtype);
    if (!impls) {
      return false;
    }
    return domain_ == DirtyDomain::RealTime
               ? static_cast<bool>(impls->tiled_rt)
               : static_cast<bool>(impls->tiled_hp);
  }

  /** @brief Returns the task kind used for non-tiled graph-backed execution. */
  PlannedTaskKind scalar_task_kind(const Node& node) const {
    const auto* impls =
        OpRegistry::instance().get_implementations(node.type, node.subtype);
    if (!impls) {
      return PlannedTaskKind::Node;
    }
    return static_cast<bool>(impls->monolithic_hp) ? PlannedTaskKind::Monolithic
                                                   : PlannedTaskKind::Node;
  }

  /** @brief Returns current tile size preference for tiled graph expansion. */
  int tile_size_for_node(const Node& node) const {
    int tile_size = 128;
    auto meta = OpRegistry::instance().get_metadata(node.type, node.subtype);
    if (!meta) {
      return tile_size;
    }
    if (meta->tile_preference == TileSizePreference::MICRO) {
      return 16;
    }
    if (meta->tile_preference == TileSizePreference::MACRO) {
      return 256;
    }
    return tile_size;
  }

 private:
  /** @brief Domain whose implementation slots are used for task shape. */
  DirtyDomain domain_;
};

/**
 * @brief Creates a PlannedTask with shared defaults for all branch strategies.
 */
PlannedTask make_task(int node_id, PlannedTaskKind kind, DirtyDomain domain,
                      const cv::Rect& output_roi, int tile_x, int tile_y,
                      int tile_size, bool whole_output) {
  PlannedTask task;
  task.node_id = node_id;
  task.kind = kind;
  task.domain = domain;
  task.output_roi = output_roi;
  task.tile_x = tile_x;
  task.tile_y = tile_y;
  task.tile_size = tile_size;
  task.whole_output = whole_output;
  return task;
}

/**
 * @brief Appends tasks while maintaining task ids and per-node task lists.
 */
class TaskAppender {
 public:
  /** @brief Binds the appender to the plan and optional dirty snapshot. */
  TaskAppender(ComputePlan& result, const DirtyRegionSnapshot* snapshot)
      : result_(result), snapshot_(snapshot) {}

  /** @brief Adds a task and updates PlannedNodeWork::task_ids when present. */
  void add(PlannedTask task) {
    task.task_id = static_cast<int>(result_.task_graph.tasks.size());
    apply_task_dirty_metadata(task, snapshot_);
    auto work_it =
        std::find_if(result_.planned_work.begin(), result_.planned_work.end(),
                     [&](const PlannedNodeWork& work) {
                       return work.node_id == task.node_id;
                     });
    if (work_it != result_.planned_work.end()) {
      work_it->task_ids.push_back(task.task_id);
    }
    result_.task_graph.tasks.push_back(std::move(task));
  }

 private:
  /** @brief Plan whose task graph is being populated. */
  ComputePlan& result_;

  /** @brief Optional dirty snapshot used for per-task metadata. */
  const DirtyRegionSnapshot* snapshot_;
};

/**
 * @brief Populates dirty snapshot tile/monolithic tasks without graph access.
 */
class SnapshotOnlyTaskPopulationStrategy {
 public:
  /** @brief Appends snapshot tasks and fallback node tasks for planned work. */
  void populate(ComputePlan& result, const DirtyRegionSnapshot& snapshot,
                DirtyDomain domain) const {
    TaskAppender appender(result, &snapshot);
    const std::unordered_set<int> planned_set(result.planned_nodes.begin(),
                                              result.planned_nodes.end());
    append_monolithic_snapshot_tasks(result, snapshot, domain, planned_set,
                                     appender);
    append_tile_snapshot_tasks(result, snapshot, domain, planned_set, appender);
    append_missing_node_tasks(result, domain, appender);
  }

 private:
  /** @brief Appends dirty monolithic regions that are still in the plan. */
  void append_monolithic_snapshot_tasks(
      ComputePlan& result, const DirtyRegionSnapshot& snapshot,
      DirtyDomain domain, const std::unordered_set<int>& planned_set,
      TaskAppender& appender) const {
    (void)result;
    for (const auto& region : snapshot.dirty_monolithic_nodes) {
      if (region.domain != domain || !planned_set.count(region.node_id)) {
        continue;
      }
      appender.add(make_task(region.node_id, PlannedTaskKind::Monolithic,
                             domain, region.pixel_roi, -1, -1, 0,
                             region.whole_output));
    }
  }

  /** @brief Appends dirty tile records that are still in the plan. */
  void append_tile_snapshot_tasks(ComputePlan& result,
                                  const DirtyRegionSnapshot& snapshot,
                                  DirtyDomain domain,
                                  const std::unordered_set<int>& planned_set,
                                  TaskAppender& appender) const {
    (void)result;
    for (const auto& tile : snapshot.dirty_tiles) {
      if (tile.domain != domain || !planned_set.count(tile.node_id)) {
        continue;
      }
      appender.add(make_task(tile.node_id, PlannedTaskKind::Tile, domain,
                             tile.pixel_roi, tile.tile_x, tile.tile_y,
                             tile.tile_size, false));
    }
  }

  /** @brief Appends node fallback tasks for planned work with no explicit task.
   */
  void append_missing_node_tasks(ComputePlan& result, DirtyDomain domain,
                                 TaskAppender& appender) const {
    for (const auto& work : result.planned_work) {
      if (!work.task_ids.empty()) {
        continue;
      }
      appender.add(make_task(work.node_id, PlannedTaskKind::Node, domain,
                             work.execution_roi, -1, -1, 0, work.whole_output));
    }
  }
};

/**
 * @brief Populates simple node tasks when neither graph nor snapshot is known.
 */
class NodeOnlyTaskPopulationStrategy {
 public:
  /** @brief Appends one node task per planned work item. */
  void populate(ComputePlan& result, const DirtyRegionSnapshot* snapshot,
                DirtyDomain domain) const {
    TaskAppender appender(result, snapshot);
    for (const auto& work : result.planned_work) {
      appender.add(make_task(work.node_id, PlannedTaskKind::Node, domain,
                             work.execution_roi, -1, -1, 0, work.whole_output));
    }
  }
};

/**
 * @brief Populates full graph-backed tile, monolithic, or node tasks.
 */
class GraphTaskPopulationStrategy {
 public:
  /** @brief Resolves graph extents and emits executable tasks per node. */
  void populate(ComputePlan& result, const DirtyRegionSnapshot* snapshot,
                DirtyDomain domain, const GraphModel& graph) const {
    TaskAppender appender(result, snapshot);
    DomainTaskShapeStrategy shape_strategy(domain);
    GraphExtentResolver extent_resolver;
    std::unordered_map<int, cv::Size> extent_cache;
    for (const auto& work : result.planned_work) {
      if (!graph.has_node(work.node_id)) {
        continue;
      }
      append_graph_tasks_for_work(result, graph, work, domain, shape_strategy,
                                  extent_resolver, extent_cache, appender);
    }
  }

 private:
  /** @brief Appends all task shapes for a single graph-backed work item. */
  void append_graph_tasks_for_work(
      ComputePlan& result, const GraphModel& graph, const PlannedNodeWork& work,
      DirtyDomain domain, const DomainTaskShapeStrategy& shape_strategy,
      GraphExtentResolver& extent_resolver,
      std::unordered_map<int, cv::Size>& extent_cache,
      TaskAppender& appender) const {
    (void)result;
    const Node& node = graph.node(work.node_id);
    cv::Size extent = extent_resolver.resolve_output_extent(
        const_cast<GraphModel&>(graph), work.node_id, extent_cache);
    cv::Rect full_output(0, 0, std::max(0, extent.width),
                         std::max(0, extent.height));
    if (shape_strategy.has_tiled_impl(node) && full_output.width > 0 &&
        full_output.height > 0) {
      append_tiled_tasks(work, domain, full_output, shape_strategy, node,
                         appender);
      return;
    }

    const PlannedTaskKind kind = shape_strategy.scalar_task_kind(node);
    const cv::Rect output_roi =
        full_output.width > 0 ? full_output : work.execution_roi;
    appender.add(make_task(work.node_id, kind, domain, output_roi, -1, -1, 0,
                           kind == PlannedTaskKind::Monolithic));
  }

  /** @brief Splits one full output extent into domain-local tile tasks. */
  void append_tiled_tasks(const PlannedNodeWork& work, DirtyDomain domain,
                          const cv::Rect& full_output,
                          const DomainTaskShapeStrategy& shape_strategy,
                          const Node& node, TaskAppender& appender) const {
    const int tile_size = shape_strategy.tile_size_for_node(node);
    for (int y = 0; y < full_output.height; y += tile_size) {
      for (int x = 0; x < full_output.width; x += tile_size) {
        cv::Rect tile_roi(x, y, std::min(tile_size, full_output.width - x),
                          std::min(tile_size, full_output.height - y));
        appender.add(make_task(work.node_id, PlannedTaskKind::Tile, domain,
                               tile_roi, x / tile_size, y / tile_size,
                               tile_size, false));
      }
    }
  }
};

}  // namespace

void apply_task_dirty_metadata(PlannedTask& task,
                               const DirtyRegionSnapshot* snapshot) {
  task.source_boundary_eligible = is_dirty_source(snapshot, task.node_id);
  task.dirty_generation = snapshot ? snapshot->graph_generation : 0;
  task.dirty_selected = intersects_dirty_roi(task, snapshot);
}

void TaskPopulationStrategy::populate(ComputePlan& result,
                                      const DirtyRegionSnapshot* snapshot,
                                      DirtyDomain domain,
                                      const GraphModel* graph) const {
  if (!graph && snapshot) {
    SnapshotOnlyTaskPopulationStrategy{}.populate(result, *snapshot, domain);
    return;
  }
  if (!graph) {
    NodeOnlyTaskPopulationStrategy{}.populate(result, snapshot, domain);
    return;
  }
  GraphTaskPopulationStrategy{}.populate(result, snapshot, domain, *graph);
}

}  // namespace ps::compute
