#include "compute/task_graph_planning.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "compute/compute_cache_policy.hpp"
#include "compute/compute_geometry.hpp"
#include "compute/domain_op_metadata.hpp"
#include "compute/node_executor.hpp"
#include "compute/task_population_strategy.hpp"
#include "graph/graph_extent_resolver.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {
namespace {

/** @brief Task-shape config token used by FullTaskGraph cache keys. */
constexpr const char* kTaskShapeConfigVersion = "task-shape-v2";

/** @brief Maximum attempts to observe one stable operation-registry shape. */
constexpr int kMaxRegistryStableExpansionAttempts = 8;

/**
 * @brief Builds a FullTaskGraph cache key for one captured registry generation.
 *
 * @param graph Graph supplying the topology generation.
 * @param intent HP or RT planning domain.
 * @param registry_generation Captured operation-registry task-shape revision.
 * @return Stable key for these planning inputs and the shape configuration.
 * @throws std::bad_alloc if string construction allocates.
 * @note Callers must verify that `registry_generation` remains current before
 *       returning a cached or newly expanded graph.
 */
std::string make_full_task_graph_cache_key(const GraphModel& graph,
                                           ComputeIntent intent,
                                           std::uint64_t registry_generation) {
  return std::to_string(graph.topology_generation()) + ":" +
         std::to_string(static_cast<int>(intent)) + ":" +
         std::to_string(registry_generation) + ":" + kTaskShapeConfigVersion;
}

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
 * @brief Returns whether a rectangle contains usable ROI information.
 *
 * @param roi Rectangle supplied by a task or dependency mapping.
 * @return True when width and height are positive.
 * @throws Nothing.
 * @note Empty dependency ROIs mean "unknown/full dependency" rather than an
 * empty spatial relationship.
 */
bool has_roi(const cv::Rect& roi) {
  return roi.width > 0 && roi.height > 0;
}

/**
 * @brief Returns whether two task ROIs overlap.
 *
 * @param upstream_roi Candidate producer ROI.
 * @param downstream_roi Candidate consumer input ROI.
 * @return True when the rectangles overlap with positive area.
 * @throws Nothing.
 * @note Empty rectangles are treated by callers before this helper is used.
 */
bool rois_overlap(const cv::Rect& upstream_roi,
                  const cv::Rect& downstream_roi) {
  return (upstream_roi & downstream_roi).area() > 0;
}

/**
 * @brief Builds a stable integer key for a tile grid coordinate.
 *
 * @param tile_x Tile x coordinate from PlannedTask.
 * @param tile_y Tile y coordinate from PlannedTask.
 * @return Packed coordinate key suitable for unordered_map lookup.
 * @throws Nothing.
 * @note Tile coordinates are non-negative planner ids, not pixel positions.
 */
std::int64_t tile_coordinate_key(int tile_x, int tile_y) {
  return (static_cast<std::int64_t>(tile_x) << 32) ^
         static_cast<std::uint32_t>(tile_y);
}

/**
 * @brief Tile lookup for one upstream node inside a task graph.
 *
 * The index stores all task ids plus a grid map for tiled nodes whose tasks
 * have consistent positive tile sizes and coordinates. Grid lookup lets
 * dependency construction enumerate only tiles covered by an input ROI.
 */
struct NodeTaskDependencyIndex {
  /** @brief All task ids owned by the node in stable task order. */
  std::vector<int> task_ids;
  /** @brief Task ids that are not tile-grid addressable. */
  std::vector<int> non_grid_task_ids;
  /** @brief Tile size shared by grid-addressable tile tasks. */
  int tile_size = 0;
  /** @brief Whether tile_by_coordinate can be used for ROI range lookup. */
  bool grid_addressable = true;
  /** @brief Tile task id keyed by tile_x/tile_y coordinate. */
  std::unordered_map<std::int64_t, int> tile_by_coordinate;
};

/**
 * @brief Appends one task to a per-node dependency index.
 *
 * @param task Task being indexed.
 * @param index Node-local task index being updated.
 * @throws std::bad_alloc if vectors or maps grow.
 * @note Mixed tile sizes or missing tile coordinates disable range lookup for
 * that node; downstream tasks then depend on the node-level producer tasks.
 */
void add_task_to_dependency_index(const PlannedTask& task,
                                  NodeTaskDependencyIndex& index) {
  index.task_ids.push_back(task.task_id);
  if (task.kind != PlannedTaskKind::Tile || task.tile_size <= 0 ||
      task.tile_x < 0 || task.tile_y < 0 || !has_roi(task.output_roi)) {
    index.non_grid_task_ids.push_back(task.task_id);
    return;
  }
  if (index.tile_size == 0) {
    index.tile_size = task.tile_size;
  } else if (index.tile_size != task.tile_size) {
    index.grid_addressable = false;
  }
  index.tile_by_coordinate[tile_coordinate_key(task.tile_x, task.tile_y)] =
      task.task_id;
}

/**
 * @brief Builds node-keyed task indexes for dependency construction.
 *
 * @param tasks Dense task list from a ComputeTaskGraph.
 * @return Map from node id to task id and tile-grid lookup metadata.
 * @throws std::bad_alloc if index storage grows.
 * @note The returned indexes never mutate tasks and can be reused for both
 * full plans and dirty overlay dependency views.
 */
std::unordered_map<int, NodeTaskDependencyIndex> build_task_dependency_index(
    const std::vector<PlannedTask>& tasks) {
  std::unordered_map<int, NodeTaskDependencyIndex> indexes;
  for (const auto& task : tasks) {
    add_task_to_dependency_index(task, indexes[task.node_id]);
  }
  for (auto& [_, index] : indexes) {
    if (!index.non_grid_task_ids.empty() || index.tile_size <= 0 ||
        index.tile_by_coordinate.empty()) {
      index.grid_addressable = false;
    }
  }
  return indexes;
}

/**
 * @brief Resolves one node output extent for planning-time ROI inference.
 *
 * @param graph Graph whose node extents are resolved.
 * @param node_id Node id to resolve.
 * @param resolver Shared resolver used by the dependency builder.
 * @param extent_cache Request-local extent cache.
 * @return Resolved output size, or an empty size for invalid ids.
 * @throws GraphError or standard exceptions from GraphExtentResolver.
 * @note The cache is local to dependency construction and does not persist in
 * GraphModel.
 */
cv::Size resolve_planned_extent(
    const GraphModel& graph, int node_id, GraphExtentResolver& resolver,
    std::unordered_map<int, cv::Size>& extent_cache) {
  if (!graph.has_node(node_id)) {
    return cv::Size();
  }
  return resolver.resolve_output_extent(graph, node_id, extent_cache);
}

/**
 * @brief Resolves every declared image-input extent for one planned node.
 * @param graph Graph containing downstream image-input topology.
 * @param node Downstream node whose input-index space is preserved.
 * @param resolver Shared extent resolver for this planning request.
 * @param extent_cache Request-local output-extent cache.
 * @return Extents indexed exactly like node.image_inputs, with empty values for
 *         disconnected or unavailable parents.
 * @throws GraphError or standard exceptions from extent resolution.
 * @throws std::bad_alloc when vector storage allocation fails.
 * @note This captures all graph-known inputs, not only the edge currently being
 *       mapped, so public random-access RoiContext snapshots remain complete.
 */
std::vector<cv::Size> resolve_planned_input_extents(
    const GraphModel& graph, const Node& node, GraphExtentResolver& resolver,
    std::unordered_map<int, cv::Size>& extent_cache) {
  std::vector<cv::Size> extents(node.image_inputs.size());
  for (std::size_t index = 0; index < node.image_inputs.size(); ++index) {
    const int source_id = node.image_inputs[index].from_node_id;
    if (source_id >= 0 && graph.has_node(source_id)) {
      extents[index] =
          resolve_planned_extent(graph, source_id, resolver, extent_cache);
    }
  }
  return extents;
}

/**
 * @brief Infers the input ROI consumed by one downstream tile task.
 *
 * @param dependency Node-level edge whose upstream node provides the input.
 * @param to_task Downstream tile task being planned.
 * @param graph Optional graph used to match NodeExecutor ROI behavior.
 * @param resolver Shared extent resolver for graph-backed inference.
 * @param extent_cache Request-local extent cache.
 * @return Upstream ROI required by to_task, or an empty rectangle when unknown.
 * @throws GraphError or propagator exceptions from graph-backed ROI mapping.
 * @note When graph is present this calls NodeExecutor::input_roi_for_tile() so
 *       halo and RandomAccess operators match the execution path. A non-empty
 *       snapshot `dependency.from_roi` is then unioned as a conservative lower
 *       bound, so dirty forced-halo execution cannot read producer tiles it did
 *       not wait for. Effective
 *       upstream parameter values are resolved only for RandomAccess because
 *       aligned and halo geometry does not consume them; this lets a first
 *       request plan its still-uncached parameter producer. Without graph, the
 *       function falls back to dependency ROI metadata or aligned output ROI.
 */
cv::Rect required_upstream_roi_for_task(
    const PlannedDependency& dependency, const PlannedTask& to_task,
    const GraphModel* graph, GraphExtentResolver& resolver,
    std::unordered_map<int, cv::Size>& extent_cache) {
  if (graph && graph->has_node(dependency.from_node_id) &&
      graph->has_node(dependency.to_node_id) && has_roi(to_task.output_roi)) {
    const cv::Size upstream_extent = resolve_planned_extent(
        *graph, dependency.from_node_id, resolver, extent_cache);
    if (upstream_extent.width > 0 && upstream_extent.height > 0) {
      ImageBuffer input_buffer;
      input_buffer.width = upstream_extent.width;
      input_buffer.height = upstream_extent.height;
      input_buffer.channels = 1;
      input_buffer.type = DataType::FLOAT32;

      TiledExecutionConfig config;
      config.tile_size = to_task.tile_size > 0 ? to_task.tile_size : 256;
      config.output_roi = to_task.output_roi;
      const cv::Size downstream_extent = resolve_planned_extent(
          *graph, dependency.to_node_id, resolver, extent_cache);
      if (downstream_extent.width > 0 && downstream_extent.height > 0) {
        config.output_size = downstream_extent;
      }
      const Node& downstream_node = graph->node(dependency.to_node_id);
      const std::vector<cv::Size> input_extents = resolve_planned_input_extents(
          *graph, downstream_node, resolver, extent_cache);
      std::optional<OpMetadata> metadata = metadata_for_domain(
          downstream_node.type, downstream_node.subtype, to_task.domain);
      if (metadata) {
        config.metadata = *metadata;
      }
      std::optional<plugin::ParameterMap> effective_parameters;
      if (metadata && metadata->access_pattern ==
                          OpMetadata::InputAccessPattern::RandomAccess) {
        effective_parameters =
            resolve_effective_parameter_snapshot(downstream_node, *graph);
      }
      const cv::Rect execution_mapped_roi = NodeExecutor::input_roi_for_tile(
          const_cast<GraphModel&>(*graph), downstream_node, to_task.output_roi,
          input_buffer, config, input_extents,
          effective_parameters ? &*effective_parameters : nullptr);
      const cv::Rect snapshot_lower_bound =
          clip_rect(dependency.from_roi, upstream_extent);
      return clip_rect(
          merge_optional_rect(execution_mapped_roi, snapshot_lower_bound),
          upstream_extent);
    }
  }

  if (has_roi(dependency.from_roi)) {
    return dependency.from_roi;
  }
  return to_task.output_roi;
}

/**
 * @brief Appends upstream tile ids whose output tiles overlap a required ROI.
 *
 * @param dependency_ids Destination dependency ids for one downstream task.
 * @param upstream_index Grid-addressable upstream task index.
 * @param required_roi Upstream input ROI consumed by the downstream task.
 * @param tasks Dense task list used to verify edge-tile overlap.
 * @throws std::bad_alloc if dependency_ids grows.
 * @note The helper enumerates coordinate ranges directly and performs a final
 * ROI overlap check only for edge tiles.
 */
void append_covering_upstream_tiles(
    std::vector<int>& dependency_ids,
    const NodeTaskDependencyIndex& upstream_index, const cv::Rect& required_roi,
    const std::vector<PlannedTask>& tasks) {
  if (!upstream_index.grid_addressable || upstream_index.tile_size <= 0) {
    return;
  }
  if (!has_roi(required_roi)) {
    return;
  }
  const int tile_size = upstream_index.tile_size;
  const int x_begin = std::max(0, required_roi.x) / tile_size;
  const int y_begin = std::max(0, required_roi.y) / tile_size;
  const int x_last =
      std::max(0, required_roi.x + required_roi.width - 1) / tile_size;
  const int y_last =
      std::max(0, required_roi.y + required_roi.height - 1) / tile_size;
  for (int tile_y = y_begin; tile_y <= y_last; ++tile_y) {
    for (int tile_x = x_begin; tile_x <= x_last; ++tile_x) {
      const auto tile_it = upstream_index.tile_by_coordinate.find(
          tile_coordinate_key(tile_x, tile_y));
      if (tile_it == upstream_index.tile_by_coordinate.end()) {
        continue;
      }
      const int from_task_id = tile_it->second;
      if (from_task_id < 0 || from_task_id >= static_cast<int>(tasks.size())) {
        continue;
      }
      const PlannedTask& from_task = tasks[from_task_id];
      if (!rois_overlap(from_task.output_roi, required_roi)) {
        continue;
      }
      append_unique(dependency_ids, from_task_id);
    }
  }
}

/**
 * @brief Determines whether a task pair can use spatial tile dependency logic.
 *
 * @param dependency Node-level dependency under consideration.
 * @param upstream_index Task index for dependency.from_node_id.
 * @param to_task Downstream task being populated.
 * @return True when the edge can be reduced to ROI-covered upstream tiles.
 * @throws Nothing.
 * @note Non-image edges and non-tile downstream work keep whole-node
 * dependency semantics.
 */
bool can_use_spatial_tile_dependency(
    const PlannedDependency& dependency,
    const NodeTaskDependencyIndex& upstream_index, const PlannedTask& to_task) {
  return dependency.input_kind == "image" && upstream_index.grid_addressable &&
         to_task.kind == PlannedTaskKind::Tile && has_roi(to_task.output_roi);
}

/**
 * @brief Detects image edges whose request-time ROI cannot be known at plan
 * construction.
 * @param dependency Candidate image dependency to a tiled consumer.
 * @param to_task Downstream task whose intent selects domain metadata.
 * @param graph Optional graph containing parameter-input topology.
 * @return True when the downstream random-access operator has at least one
 * connected parameter input whose same-request value may differ from the
 * committed snapshot.
 * @throws std::bad_alloc or registry exceptions from metadata lookup.
 * @note Full task graphs are cached across requests. Conservatively retaining
 * every upstream image tile for this shape prevents an old narrow parameter
 * value from releasing the consumer before newly required outer tiles finish.
 */
bool requires_conservative_parameterized_image_dependency(
    const PlannedDependency& dependency, const PlannedTask& to_task,
    const GraphModel* graph) {
  if (!graph || dependency.input_kind != "image" ||
      !graph->has_node(dependency.to_node_id)) {
    return false;
  }
  const Node& downstream = graph->node(dependency.to_node_id);
  const bool has_connected_parameter = std::any_of(
      downstream.parameter_inputs.begin(), downstream.parameter_inputs.end(),
      [](const ParameterInput& input) { return input.from_node_id >= 0; });
  if (!has_connected_parameter) {
    return false;
  }
  const auto metadata =
      metadata_for_domain(downstream.type, downstream.subtype, to_task.domain);
  return metadata && metadata->access_pattern ==
                         OpMetadata::InputAccessPattern::RandomAccess;
}

/**
 * @brief Appends every producer task for a node-level dependency.
 *
 * @param dependency_ids Destination dependency ids for one downstream task.
 * @param upstream_index Upstream node task index.
 * @throws std::bad_alloc if dependency_ids grows.
 * @note This is used for parameter edges, monolithic producers, and downstream
 * tasks that execute a whole output rather than a tile ROI.
 */
void append_node_dependency_tasks(
    std::vector<int>& dependency_ids,
    const NodeTaskDependencyIndex& upstream_index) {
  for (int from_task_id : upstream_index.task_ids) {
    append_unique(dependency_ids, from_task_id);
  }
}

/**
 * @brief Builds dependency ids from node dependencies and optional graph hints.
 *
 * @param tasks Dense task graph tasks to inspect.
 * @param dependencies Node-level dependencies to lower to task ids.
 * @param graph Optional graph used for execution-accurate tile input ROI.
 * @return Dependency task ids aligned with tasks by dense task id.
 * @throws GraphError, std::out_of_range, or standard allocation exceptions.
 * @note Tiled image edges enumerate upstream tile ranges directly. Whole-node,
 * parameter, non-grid, and parameterized random-access producer dependencies
 * attach the upstream node's task ids without constructing a Cartesian task
 * pair scan. The conservative random-access case is required because its
 * request-effective parameter output does not exist at planning time.
 */
std::vector<std::vector<int>> build_task_dependency_ids(
    const std::vector<PlannedTask>& tasks,
    const std::vector<PlannedDependency>& dependencies,
    const GraphModel* graph) {
  std::vector<std::vector<int>> dependency_ids(tasks.size());
  const auto task_index_by_node = build_task_dependency_index(tasks);
  GraphExtentResolver extent_resolver;
  std::unordered_map<int, cv::Size> extent_cache;

  for (const auto& dependency : dependencies) {
    const auto from_it = task_index_by_node.find(dependency.from_node_id);
    const auto to_it = task_index_by_node.find(dependency.to_node_id);
    if (from_it == task_index_by_node.end() ||
        to_it == task_index_by_node.end()) {
      continue;
    }
    const NodeTaskDependencyIndex& upstream_index = from_it->second;
    const NodeTaskDependencyIndex& downstream_index = to_it->second;
    for (int to_task_id : downstream_index.task_ids) {
      if (to_task_id < 0 || to_task_id >= static_cast<int>(tasks.size())) {
        continue;
      }
      const PlannedTask& to_task = tasks[to_task_id];
      if (has_roi(dependency.to_roi) &&
          (!has_roi(to_task.output_roi) ||
           !rois_overlap(to_task.output_roi, dependency.to_roi))) {
        continue;
      }
      std::vector<int>& ids = dependency_ids.at(to_task_id);
      if (can_use_spatial_tile_dependency(dependency, upstream_index,
                                          to_task) &&
          !requires_conservative_parameterized_image_dependency(
              dependency, to_task, graph)) {
        const cv::Rect required_roi = required_upstream_roi_for_task(
            dependency, to_task, graph, extent_resolver, extent_cache);
        append_covering_upstream_tiles(ids, upstream_index, required_roi,
                                       tasks);
        continue;
      }
      append_node_dependency_tasks(ids, upstream_index);
    }
  }

  return dependency_ids;
}

/**
 * @brief Builds task-level dependency ids and initial ready task ids.
 *
 * @param result Plan whose ComputeTaskGraph task dependency metadata is
 * refreshed.
 * @throws std::bad_alloc if node-to-task lookup or ready ids grow.
 * @note Tile-to-tile image edges depend on the downstream input ROI. Whole-node
 * and parameter dependencies attach producer task ids directly.
 */
void populate_task_dependencies(ComputePlan& result, const GraphModel* graph) {
  result.task_graph.initial_task_ids.clear();
  std::unordered_set<int> dependent_task_ids;
  std::vector<std::vector<int>> dependency_ids = build_task_dependency_ids(
      result.task_graph.tasks, result.task_graph.dependencies, graph);
  for (auto& task : result.task_graph.tasks) {
    if (task.task_id < 0 ||
        task.task_id >= static_cast<int>(dependency_ids.size())) {
      task.dependency_task_ids.clear();
      continue;
    }
    task.dependency_task_ids = std::move(dependency_ids[task.task_id]);
    if (!task.dependency_task_ids.empty()) {
      dependent_task_ids.insert(task.task_id);
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
 * pruning, and dirty snapshot inspection.
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
  populate_task_dependencies(result, graph);
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
    if (source_nodes.count(task.node_id) && task.dirty_selected) {
      work_set.dirty_source_task_ids.push_back(task.task_id);
    }
  }
  return work_set;
}

/**
 * @brief Returns an overlay node selection record, creating it from work.
 *
 * @param overlay Dirty task selection receiving node ROI metadata.
 * @param work Planned work record whose node id seeds a missing selection.
 * @return Mutable node selection keyed by work.node_id.
 * @throws std::bad_alloc if the unordered_map grows.
 * @note The helper stores only dirty-generation metadata, not full
 * PlannedNodeWork.
 */
DirtyNodeSelection& ensure_node_selection(DirtyTaskSelectionOverlay& overlay,
                                          const PlannedNodeWork& work) {
  auto [it, _] = overlay.node_selections.try_emplace(work.node_id);
  DirtyNodeSelection& selection = it->second;
  selection.node_id = work.node_id;
  return selection;
}

/**
 * @brief Looks up planned node work by node id for overlay ROI selection.
 *
 * @param plan Immutable node/cache-pruned plan being viewed.
 * @return Map from node id to PlannedNodeWork pointer.
 * @throws std::bad_alloc if lookup storage grows.
 * @note Pointers remain valid for the lifetime of plan.
 */
std::unordered_map<int, const PlannedNodeWork*> planned_work_by_node(
    const ComputePlan& plan) {
  std::unordered_map<int, const PlannedNodeWork*> work_by_node;
  work_by_node.reserve(plan.planned_work.size());
  for (const auto& work : plan.planned_work) {
    work_by_node[work.node_id] = &work;
  }
  return work_by_node;
}

/**
 * @brief Applies dirty ROI metadata to an overlay instead of a plan copy.
 *
 * @param plan Immutable node/cache-pruned plan whose nodes are eligible.
 * @param snapshot Dirty snapshot for the current generation.
 * @param domain HP or RT domain being selected.
 * @param overlay Overlay receiving per-node ROI metadata.
 * @throws std::bad_alloc if overlay maps or vectors grow.
 * @note This mirrors populate_node_regions() without clearing or copying
 * PlannedNodeWork records.
 */
void populate_overlay_node_regions(const ComputePlan& plan,
                                   const DirtyRegionSnapshot& snapshot,
                                   DirtyDomain domain,
                                   DirtyTaskSelectionOverlay& overlay) {
  const auto work_by_node = planned_work_by_node(plan);

  for (const auto& [node_id, rois] : snapshot.per_node_dirty_rois) {
    auto work_it = work_by_node.find(node_id);
    if (work_it == work_by_node.end())
      continue;
    DirtyNodeSelection& selection =
        ensure_node_selection(overlay, *work_it->second);
    for (const auto& roi : rois) {
      selection.dirty_rois.push_back(roi);
      selection.represented_hp_roi =
          merge_optional_rect(selection.represented_hp_roi, roi);
      if (domain == DirtyDomain::HighPrecision) {
        selection.execution_roi =
            merge_optional_rect(selection.execution_roi, roi);
      }
    }
  }

  for (const auto& tile : snapshot.dirty_tiles) {
    if (tile.domain != domain)
      continue;
    auto work_it = work_by_node.find(tile.node_id);
    if (work_it == work_by_node.end())
      continue;
    DirtyNodeSelection& selection =
        ensure_node_selection(overlay, *work_it->second);
    selection.execution_roi =
        merge_optional_rect(selection.execution_roi, tile.pixel_roi);
  }

  for (const auto& region : snapshot.dirty_monolithic_nodes) {
    if (region.domain != domain)
      continue;
    auto work_it = work_by_node.find(region.node_id);
    if (work_it == work_by_node.end())
      continue;
    DirtyNodeSelection& selection =
        ensure_node_selection(overlay, *work_it->second);
    selection.whole_output = selection.whole_output || region.whole_output;
    selection.execution_roi =
        merge_optional_rect(selection.execution_roi, region.pixel_roi);
    if (domain == DirtyDomain::HighPrecision) {
      selection.represented_hp_roi =
          merge_optional_rect(selection.represented_hp_roi, region.pixel_roi);
    }
  }
}

/**
 * @brief Merges snapshot dependency ROI mappings without copying a plan.
 *
 * @param plan Immutable node/cache-pruned plan whose dependencies are used.
 * @param snapshot Dirty snapshot supplying ROI edge mappings.
 * @param domain HP or RT dependency domain.
 * @return Dependency records used by the generation-local overlay.
 * @throws std::bad_alloc if dependency storage or planned-node set grows.
 * @note Snapshot mappings outside the selected plan or different domain are
 * ignored so sibling HP/RT paths remain independent.
 */
std::vector<PlannedDependency> merged_overlay_dependencies(
    const ComputePlan& plan, const DirtyRegionSnapshot& snapshot,
    DirtyDomain domain) {
  std::vector<PlannedDependency> dependencies = plan.task_graph.dependencies;
  std::unordered_set<int> planned_set(plan.planned_nodes.begin(),
                                      plan.planned_nodes.end());
  for (const auto& edge : snapshot.edge_mappings) {
    if (edge.domain != domain)
      continue;
    if (!planned_set.count(edge.from_node_id) ||
        !planned_set.count(edge.to_node_id)) {
      continue;
    }
    add_dependency(
        dependencies,
        PlannedDependency{edge.from_node_id, edge.to_node_id, domain, "image",
                          edge.from_roi, edge.to_roi, edge.direction});
  }
  return dependencies;
}

/**
 * @brief Builds task dependency ids from explicit dependency records.
 *
 * @param plan Immutable task graph whose tasks are inspected.
 * @param dependencies Node-level dependencies with optional ROI mappings.
 * @param graph Optional graph used for execution-accurate tile input ROI.
 * @return Dependency ids aligned with plan.task_graph.tasks.
 * @throws std::bad_alloc or std::out_of_range on inconsistent task ids.
 * @note The helper is the overlay equivalent of populate_task_dependencies();
 * it shares the same tile range builder and does not mutate PlannedTask
 * records.
 */
std::vector<std::vector<int>> build_dependency_ids_for_view(
    const ComputePlan& plan, const std::vector<PlannedDependency>& dependencies,
    const GraphModel* graph) {
  return build_task_dependency_ids(plan.task_graph.tasks, dependencies, graph);
}

/**
 * @brief Finds initially ready task ids inside an active dependency view.
 *
 * @param active_task_ids Active task ids to inspect in stable order.
 * @param dependency_ids Dependency task ids aligned with task id.
 * @return Active tasks whose dependencies are outside active_task_ids.
 * @throws std::bad_alloc if temporary sets or output vectors grow.
 * @note Dependencies outside the active view are treated as already satisfied,
 * matching dirty source-before-downstream semantics.
 */
std::vector<int> initial_ready_task_ids_for_view(
    const std::vector<int>& active_task_ids,
    const std::vector<std::vector<int>>& dependency_ids) {
  std::unordered_set<int> active(active_task_ids.begin(),
                                 active_task_ids.end());
  std::vector<int> ready;
  ready.reserve(active_task_ids.size());
  for (int task_id : active_task_ids) {
    if (task_id < 0 || task_id >= static_cast<int>(dependency_ids.size())) {
      continue;
    }
    bool has_active_dependency = false;
    for (int dependency_id : dependency_ids[task_id]) {
      if (active.count(dependency_id)) {
        has_active_dependency = true;
        break;
      }
    }
    if (!has_active_dependency) {
      ready.push_back(task_id);
    }
  }
  return ready;
}

}  // namespace

std::string full_task_graph_cache_key(const GraphModel& graph,
                                      ComputeIntent intent) {
  return make_full_task_graph_cache_key(
      graph, intent, OpRegistry::instance().task_shape_generation());
}

std::shared_ptr<const FullTaskGraph> get_or_expand_full_task_graph(
    GraphModel& graph, ComputeIntent intent) {
  auto& registry = OpRegistry::instance();
  for (int attempt = 0; attempt < kMaxRegistryStableExpansionAttempts;
       ++attempt) {
    const std::uint64_t registry_generation = registry.task_shape_generation();
    const std::string key =
        make_full_task_graph_cache_key(graph, intent, registry_generation);
    if (auto cached = graph.cached_full_task_graph(key)) {
      if (registry.task_shape_generation() == registry_generation) {
        return cached;
      }
      continue;
    }
    FullTaskGraphExpander expander;
    auto expanded =
        std::make_shared<FullTaskGraph>(expander.expand(graph, intent));
    if (registry.task_shape_generation() != registry_generation) {
      continue;
    }
    graph.remember_full_task_graph(key, expanded);
    if (registry.task_shape_generation() == registry_generation) {
      return expanded;
    }
  }
  throw GraphError(
      GraphErrc::ComputeError,
      "Cannot expand FullTaskGraph while operation registry keeps changing");
}

ComputePlanSummary summarize_compute_plan(
    const GraphModel& graph, const ComputePlan& compute_plan,
    const DirtyTaskSelectionOverlay* selection,
    std::shared_ptr<const ComputePlan> shared_plan) {
  ComputePlanSummary summary;
  summary.intent = compute_plan.intent;
  summary.target_node_id = compute_plan.target_node_id;
  summary.parallel = compute_plan.parallel;
  summary.topology_generation = graph.topology_generation();
  summary.full_graph_cache_key =
      full_task_graph_cache_key(graph, compute_plan.intent);
  summary.planned_node_count = compute_plan.planned_nodes.size();
  summary.task_count = compute_plan.task_graph.tasks.size();
  summary.dependency_count = compute_plan.task_graph.dependencies.size();
  summary.initial_task_count = compute_plan.task_graph.initial_task_ids.size();
  summary.active_task_count = selection ? selection->active_task_ids.size()
                                        : compute_plan.task_graph.tasks.size();
  summary.dirty_source_task_count =
      selection ? selection->dirty_source_task_ids.size() : 0;
  summary.downstream_task_count =
      selection ? selection->downstream_task_ids.size() : 0;
  summary.initial_downstream_task_count =
      selection ? selection->initial_downstream_task_ids.size() : 0;
  for (const auto& task : compute_plan.task_graph.tasks) {
    switch (task.kind) {
      case PlannedTaskKind::Tile:
        ++summary.tile_task_count;
        break;
      case PlannedTaskKind::Monolithic:
        ++summary.monolithic_task_count;
        break;
      case PlannedTaskKind::Node:
        ++summary.node_task_count;
        break;
    }
  }

  constexpr size_t kSampleLimit = 16;
  const size_t node_sample_count =
      std::min(kSampleLimit, compute_plan.planned_nodes.size());
  summary.planned_node_sample.insert(
      summary.planned_node_sample.end(), compute_plan.planned_nodes.begin(),
      compute_plan.planned_nodes.begin() +
          static_cast<std::ptrdiff_t>(node_sample_count));
  const size_t task_sample_count =
      std::min(kSampleLimit, compute_plan.task_graph.tasks.size());
  summary.task_sample.insert(
      summary.task_sample.end(), compute_plan.task_graph.tasks.begin(),
      compute_plan.task_graph.tasks.begin() +
          static_cast<std::ptrdiff_t>(task_sample_count));
  summary.shared_plan = std::move(shared_plan);
  return summary;
}

ComputePlanSummary summarize_compute_plan(
    const GraphModel& graph, const ComputePlan& compute_plan,
    std::shared_ptr<const ComputePlan> shared_plan) {
  return summarize_compute_plan(graph, compute_plan, nullptr,
                                std::move(shared_plan));
}

/**
 * @brief Builds immutable lookup indexes for a full task graph expansion.
 *
 * @param expanded Full task graph whose index fields are rebuilt.
 * @throws std::bad_alloc if any index grows.
 * @note Indexes reference vector positions and task ids owned by expanded, so
 * callers must rebuild them whenever expanded_work, tasks, or dependencies are
 * replaced.
 */
void populate_full_task_graph_indexes(FullTaskGraph& expanded) {
  expanded.work_index_by_node.clear();
  expanded.task_ids_by_node.clear();
  expanded.dependency_indices_by_to_node.clear();
  expanded.work_index_by_node.reserve(expanded.expanded_work.size());
  expanded.task_ids_by_node.reserve(expanded.expanded_work.size());
  expanded.dependency_indices_by_to_node.reserve(
      expanded.task_graph.dependencies.size());
  for (size_t i = 0; i < expanded.expanded_work.size(); ++i) {
    expanded.work_index_by_node[expanded.expanded_work[i].node_id] = i;
  }
  for (const auto& task : expanded.task_graph.tasks) {
    expanded.task_ids_by_node[task.node_id].push_back(task.task_id);
  }
  for (size_t i = 0; i < expanded.task_graph.dependencies.size(); ++i) {
    expanded
        .dependency_indices_by_to_node[expanded.task_graph.dependencies[i]
                                           .to_node_id]
        .push_back(i);
  }
}

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
  populate_full_task_graph_indexes(expanded);
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
  result.planned_work.reserve(execution_order.size());
  for (int node_id : execution_order) {
    if (!graph.has_node(node_id)) {
      throw GraphError(GraphErrc::NotFound, "Cannot prune task graph: node " +
                                                std::to_string(node_id) +
                                                " not found.");
    }
    auto full_work_it = full_graph.work_index_by_node.find(node_id);
    if (full_work_it == full_graph.work_index_by_node.end() ||
        full_work_it->second >= full_graph.expanded_work.size()) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Full task graph is missing node " + std::to_string(node_id) + ".");
    }
    PlannedNodeWork work = full_graph.expanded_work[full_work_it->second];
    work.task_ids.clear();
    work.dependency_node_ids.clear();
    work.dependent_node_ids.clear();
    work.dirty_rois.clear();
    work.reusable_cache_available =
        ComputeCachePolicy::has_reusable_output(graph.node(node_id));
    result.planned_work.push_back(std::move(work));
  }

  for (int node_id : execution_order) {
    auto task_ids_it = full_graph.task_ids_by_node.find(node_id);
    if (task_ids_it == full_graph.task_ids_by_node.end()) {
      continue;
    }
    for (int full_task_id : task_ids_it->second) {
      if (full_task_id < 0 ||
          full_task_id >=
              static_cast<int>(full_graph.task_graph.tasks.size())) {
        continue;
      }
      PlannedTask pruned_task = full_graph.task_graph.tasks[full_task_id];
      pruned_task.task_id = static_cast<int>(result.task_graph.tasks.size());
      pruned_task.dependency_task_ids.clear();
      result.task_graph.tasks.push_back(std::move(pruned_task));
    }
  }

  for (int to_node_id : execution_order) {
    auto dependency_indices_it =
        full_graph.dependency_indices_by_to_node.find(to_node_id);
    if (dependency_indices_it ==
        full_graph.dependency_indices_by_to_node.end()) {
      continue;
    }
    for (size_t dependency_index : dependency_indices_it->second) {
      if (dependency_index >= full_graph.task_graph.dependencies.size()) {
        continue;
      }
      const PlannedDependency& dependency =
          full_graph.task_graph.dependencies[dependency_index];
      if (!selected_nodes.count(dependency.from_node_id)) {
        continue;
      }
      result.task_graph.dependencies.push_back(dependency);
    }
  }

  rebuild_work_task_ids(result);
  populate_node_dependency_lists(result);
  populate_task_dependencies(result, &graph);
  return result;
}

ComputePlan DirtySnapshotTaskGraphPruner::prune(
    const ComputePlan& node_cache_plan, const DirtyRegionSnapshot& snapshot,
    const GraphModel& graph) const {
  const DirtyTaskSelectionOverlay selection =
      select(node_cache_plan, snapshot, graph);
  ComputePlan result = node_cache_plan;

  clear_dirty_work_metadata(result);
  std::unordered_map<int, size_t> work_index;
  work_index.reserve(result.planned_work.size());
  for (size_t i = 0; i < result.planned_work.size(); ++i) {
    work_index[result.planned_work[i].node_id] = i;
  }
  for (const auto& [node_id, node_selection] : selection.node_selections) {
    auto work_it = work_index.find(node_id);
    if (work_it == work_index.end())
      continue;
    PlannedNodeWork& work = result.planned_work[work_it->second];
    work.represented_hp_roi = node_selection.represented_hp_roi;
    work.execution_roi = node_selection.execution_roi;
    work.whole_output = node_selection.whole_output;
    work.dirty_rois = node_selection.dirty_rois;
  }
  result.task_graph.dependencies = selection.dependencies;
  for (auto& task : result.task_graph.tasks) {
    apply_task_dirty_metadata(task, &snapshot);
  }
  rebuild_work_task_ids(result);
  populate_node_dependency_lists(result);
  populate_task_dependencies(result, &graph);
  return result;
}

DirtyTaskSelectionOverlay DirtySnapshotTaskGraphPruner::select(
    const ComputePlan& node_cache_plan, const DirtyRegionSnapshot& snapshot,
    const GraphModel& graph,
    const std::unordered_set<int>* externally_satisfied_node_ids) const {
  DirtyTaskSelectionOverlay selection;
  selection.generation = snapshot.graph_generation;
  selection.domain = domain_for_intent(node_cache_plan.intent);
  const size_t task_count = node_cache_plan.task_graph.tasks.size();
  selection.active_task_flags.assign(task_count, false);
  selection.source_boundary_task_flags.assign(task_count, false);

  populate_overlay_node_regions(node_cache_plan, snapshot, selection.domain,
                                selection);
  selection.dependencies =
      merged_overlay_dependencies(node_cache_plan, snapshot, selection.domain);
  selection.dependency_task_ids = build_dependency_ids_for_view(
      node_cache_plan, selection.dependencies, &graph);

  std::unordered_set<int> source_nodes(snapshot.dirty_source_nodes.begin(),
                                       snapshot.dirty_source_nodes.end());
  for (const auto& task : node_cache_plan.task_graph.tasks) {
    if (task.task_id < 0 || task.task_id >= static_cast<int>(task_count)) {
      continue;
    }
    PlannedTask selected_task = task;
    selected_task.dependency_task_ids =
        selection.dependency_task_ids.at(task.task_id);
    apply_task_dirty_metadata(selected_task, &snapshot);
    selection.source_boundary_task_flags[task.task_id] =
        selected_task.source_boundary_eligible;
    if (!selected_task.dirty_selected) {
      continue;
    }
    if (externally_satisfied_node_ids &&
        externally_satisfied_node_ids->count(task.node_id)) {
      continue;
    }
    selection.active_task_flags[task.task_id] = true;
    selection.active_task_ids.push_back(task.task_id);
    if (source_nodes.count(task.node_id)) {
      selection.dirty_source_task_ids.push_back(task.task_id);
    } else {
      selection.downstream_task_ids.push_back(task.task_id);
    }
  }
  selection.initial_downstream_task_ids = initial_ready_task_ids_for_view(
      selection.downstream_task_ids, selection.dependency_task_ids);
  return selection;
}

DirtyUpdateWorkSet DirtySnapshotTaskGraphPruner::materialize(
    const DirtyTaskSelectionOverlay& selection) const {
  DirtyUpdateWorkSet work_set;
  work_set.generation = selection.generation;
  work_set.dirty_source_task_ids = selection.dirty_source_task_ids;
  work_set.downstream_task_ids = selection.downstream_task_ids;
  return work_set;
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
