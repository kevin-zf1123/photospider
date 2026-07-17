#include "compute/task_population_strategy.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <variant>

#include "compute/compute_geometry.hpp"
#include "compute/domain_op_metadata.hpp"
#include "graph/graph_extent_resolver.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {
namespace {

/**
 * @brief Merges two optional ROI values using empty-as-missing semantics.
 *
 * @param current Accumulated ROI or empty.
 * @param next ROI to include or empty.
 * @return Checked bounding union, or the non-empty operand.
 * @throws Nothing.
 * @note compute::merge_rect performs wide endpoint arithmetic and treats empty
 *       rectangles as the union identity.
 */
PixelRect merge_optional_rect(const PixelRect& current,
                              const PixelRect& next) noexcept {
  return merge_rect(current, next);
}

/**
 * @brief Checks whether one planned node is a dirty-source boundary.
 * @param snapshot Optional graph-scoped dirty snapshot.
 * @param node_id Planned node id to inspect.
 * @return True when snapshot identifies node_id as a dirty source.
 * @throws Nothing.
 * @note A null snapshot describes ordinary non-dirty task population.
 */
bool is_dirty_source(const DirtyRegionSnapshot* snapshot,
                     int node_id) noexcept {
  if (!snapshot) {
    return false;
  }
  return std::find(snapshot->dirty_source_nodes.begin(),
                   snapshot->dirty_source_nodes.end(),
                   node_id) != snapshot->dirty_source_nodes.end();
}

/**
 * @brief Conservatively merges every dirty record relevant to one node/domain.
 * @param snapshot Optional graph-scoped dirty snapshot.
 * @param node_id Node whose dirty records are collected.
 * @param domain HP or RT task domain being populated.
 * @return Checked bounding ROI, or empty when no matching record exists.
 * @throws Nothing.
 * @note Source, actual, per-node, tile, and monolithic records are value-only;
 *       the helper neither mutates the snapshot nor creates task shapes.
 */
PixelRect dirty_roi_for_node(const DirtyRegionSnapshot* snapshot, int node_id,
                             DirtyDomain domain) noexcept {
  if (!snapshot) {
    return PixelRect{};
  }
  PixelRect merged;
  auto actual_it = snapshot->actual_dirty_rois.find(node_id);
  if (actual_it != snapshot->actual_dirty_rois.end()) {
    for (const auto& roi : actual_it->second) {
      merged = merge_optional_rect(merged, roi);
    }
  }
  auto source_it = snapshot->source_roi_records.find(node_id);
  if (source_it != snapshot->source_roi_records.end()) {
    for (const auto& record : source_it->second) {
      if (record.domain == domain) {
        merged = merge_optional_rect(merged, record.source_roi);
      }
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

/**
 * @brief Decides whether one pre-expanded task intersects selected dirty work.
 * @param task Planned task whose output geometry is checked.
 * @param snapshot Optional graph-scoped dirty snapshot.
 * @return True for ordinary planning, unknown task ROI, or a positive dirty
 *         intersection; false when dirty planning selects no matching area.
 * @throws Nothing.
 * @note Intersection uses checked kernel geometry and never creates new tasks.
 */
bool intersects_dirty_roi(const PlannedTask& task,
                          const DirtyRegionSnapshot* snapshot) noexcept {
  if (!snapshot) {
    return true;
  }
  const PixelRect dirty_roi =
      dirty_roi_for_node(snapshot, task.node_id, task.domain);
  if (dirty_roi.width <= 0 || dirty_roi.height <= 0) {
    return false;
  }
  if (task.output_roi.width <= 0 || task.output_roi.height <= 0) {
    return true;
  }
  return !is_rect_empty(intersect_rect(task.output_roi, dirty_roi));
}

/**
 * @brief Derives graph task shape from the operation selected for one domain.
 *
 * @throws std::bad_alloc when registry key or callback snapshot copying cannot
 * allocate.
 * @throws Any exception raised while copying a registered callback target.
 * @note Shape selection delegates callback precedence to
 * `OpRegistry::resolve_for_intent()` so a retained predecessor slot cannot
 * override the callback that execution will actually use. A tiled callback may
 * still fall back to one full-node task when output extent is unavailable.
 */
class DomainTaskShapeStrategy {
 public:
  /**
   * @brief Binds task-shape and metadata decisions to one compute domain.
   *
   * @param domain HP or RT domain being expanded.
   * @throws Nothing.
   * @note The domain is immutable for the lifetime of this strategy.
   */
  explicit DomainTaskShapeStrategy(DirtyDomain domain) : domain_(domain) {}

  /**
   * @brief Returns the task kind selected by the registry's execution policy.
   *
   * @param node Graph node whose operation callback is resolved.
   * @return `Tile` for the selected tiled callback, `Monolithic` for the
   * selected monolithic callback, or `Node` when no callback is available.
   * @throws std::bad_alloc when key or callback snapshot copying cannot
   * allocate.
   * @throws Any exception raised while copying a registered callback target.
   * @note The returned callback snapshot is destroyed after inspecting its
   * variant, outside the registry lock. Plugin-backed snapshots retain their
   * DSO lease for that complete lifetime.
   */
  PlannedTaskKind selected_task_kind(const Node& node) const {
    const ComputeIntent intent = domain_ == DirtyDomain::RealTime
                                     ? ComputeIntent::RealTimeUpdate
                                     : ComputeIntent::GlobalHighPrecision;
    const auto selected = OpRegistry::instance().resolve_for_intent(
        node.type, node.subtype, intent);
    if (!selected) {
      return PlannedTaskKind::Node;
    }
    return std::holds_alternative<TileOpFunc>(*selected)
               ? PlannedTaskKind::Tile
               : PlannedTaskKind::Monolithic;
  }

  /**
   * @brief Resolves the tile size for the strategy's compute domain.
   *
   * @param node Graph node whose domain metadata is inspected.
   * @return 16 for Micro, 256 for Macro, or the default 128 otherwise.
   * @throws std::bad_alloc when registry key, metadata, or callback snapshot
   * copying cannot allocate.
   * @throws Any exception raised while copying a registered callback target.
   * @note HP and RT metadata remain independent; callback selection determines
   * whether this value is consumed at all.
   */
  int tile_size_for_node(const Node& node) const {
    int tile_size = 128;
    auto meta = metadata_for_domain(node.type, node.subtype, domain_);
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
  /**
   * @brief Immutable domain used for callback and metadata selection.
   * @note The strategy borrows no registry state between method calls.
   */
  DirtyDomain domain_;
};

/**
 * @brief Creates a planned task with shared defaults for every shape strategy.
 * @param node_id Graph node executed by the task.
 * @param kind Node, monolithic, or tile task kind.
 * @param domain HP or RT compute domain.
 * @param output_roi Domain-local output ROI represented by the task.
 * @param tile_x Tile column index, or -1 for non-tile work.
 * @param tile_y Tile row index, or -1 for non-tile work.
 * @param tile_size Tile edge length, or zero for non-tile work.
 * @param whole_output Whether the task represents the complete node output.
 * @return Value-only task awaiting id/dependency assignment.
 * @throws Nothing for current fixed-size field initialization.
 * @note The helper does not inspect graph state or attach dirty metadata.
 */
PlannedTask make_task(int node_id, PlannedTaskKind kind, DirtyDomain domain,
                      const PixelRect& output_roi, int tile_x, int tile_y,
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
 * @brief Populates simple node tasks when graph-backed shape data is
 * unavailable.
 */
class NodeOnlyTaskPopulationStrategy {
 public:
  /** @brief Appends one node task per planned work item with optional metadata.
   */
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
    std::unordered_map<int, PixelSize> extent_cache;
    for (const auto& work : result.planned_work) {
      if (!graph.has_node(work.node_id)) {
        continue;
      }
      append_graph_tasks_for_work(result, graph, work, domain, shape_strategy,
                                  extent_resolver, extent_cache, appender);
    }
  }

 private:
  /**
   * @brief Appends the selected operation's tasks for one graph work item.
   *
   * @param result Plan receiving tasks through `appender`.
   * @param graph Graph containing the referenced node and output extent inputs.
   * @param work Planned node work being materialized.
   * @param domain HP or RT task domain.
   * @param shape_strategy Domain-bound callback and metadata selector.
   * @param extent_resolver Resolver used to derive full output dimensions.
   * @param extent_cache Request-local extent memoization storage.
   * @param appender Task sink that assigns ids and dirty metadata.
   * @return Nothing.
   * @throws std::bad_alloc when registry snapshots, extent storage, or task
   * vectors allocate.
   * @throws GraphError and callback-copy exceptions from extent or registry
   * resolution.
   * @note A selected tiled callback expands into tiles only with a positive
   * extent. Otherwise one `Node` task preserves full-node tiled execution.
   */
  void append_graph_tasks_for_work(
      ComputePlan& result, const GraphModel& graph, const PlannedNodeWork& work,
      DirtyDomain domain, const DomainTaskShapeStrategy& shape_strategy,
      GraphExtentResolver& extent_resolver,
      std::unordered_map<int, PixelSize>& extent_cache,
      TaskAppender& appender) const {
    (void)result;
    const Node& node = graph.node(work.node_id);
    PixelSize extent = extent_resolver.resolve_output_extent(
        graph, work.node_id, extent_cache);
    PixelRect full_output{0, 0, std::max(0, extent.width),
                          std::max(0, extent.height)};
    const PlannedTaskKind selected_kind =
        shape_strategy.selected_task_kind(node);
    if (selected_kind == PlannedTaskKind::Tile && full_output.width > 0 &&
        full_output.height > 0) {
      append_tiled_tasks(work, domain, full_output, shape_strategy, node,
                         appender);
      return;
    }

    const PlannedTaskKind kind = selected_kind == PlannedTaskKind::Tile
                                     ? PlannedTaskKind::Node
                                     : selected_kind;
    const PixelRect output_roi =
        full_output.width > 0 ? full_output : work.execution_roi;
    appender.add(make_task(work.node_id, kind, domain, output_roi, -1, -1, 0,
                           kind == PlannedTaskKind::Monolithic));
  }

  /**
   * @brief Splits one positive full-output rectangle into domain-local tiles.
   * @param work Planned node work whose node id is copied to each task.
   * @param domain HP or RT task domain.
   * @param full_output Positive output bounds in domain-local pixels.
   * @param shape_strategy Domain-bound tile-size selector.
   * @param node Graph node whose operation metadata selects tile size.
   * @param appender Task sink assigning task ids and dirty metadata.
   * @return Nothing.
   * @throws std::bad_alloc when registry snapshots or task storage allocate.
   * @throws Any exception raised while copying registered metadata callbacks.
   * @note Iteration uses signed 64-bit coordinates so the final partial tile
   *       cannot overflow when a valid extent approaches INT_MAX.
   */
  void append_tiled_tasks(const PlannedNodeWork& work, DirtyDomain domain,
                          const PixelRect& full_output,
                          const DomainTaskShapeStrategy& shape_strategy,
                          const Node& node, TaskAppender& appender) const {
    const int tile_size = shape_strategy.tile_size_for_node(node);
    for (std::int64_t y = 0; y < full_output.height; y += tile_size) {
      for (std::int64_t x = 0; x < full_output.width; x += tile_size) {
        PixelRect tile_roi{
            static_cast<int>(x), static_cast<int>(y),
            static_cast<int>(std::min<std::int64_t>(
                tile_size, static_cast<std::int64_t>(full_output.width) - x)),
            static_cast<int>(std::min<std::int64_t>(
                tile_size, static_cast<std::int64_t>(full_output.height) - y))};
        appender.add(make_task(work.node_id, PlannedTaskKind::Tile, domain,
                               tile_roi, static_cast<int>(x / tile_size),
                               static_cast<int>(y / tile_size), tile_size,
                               false));
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
  if (!graph) {
    NodeOnlyTaskPopulationStrategy{}.populate(result, snapshot, domain);
    return;
  }
  GraphTaskPopulationStrategy{}.populate(result, snapshot, domain, *graph);
}

}  // namespace ps::compute
