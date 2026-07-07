#include "kernel/services/roi_propagation_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <utility>

#include "kernel/ops.hpp"

namespace ps {

namespace {

bool is_rect_empty(const cv::Rect& rect) {
  return rect.width <= 0 || rect.height <= 0;
}

cv::Rect merge_rect(const cv::Rect& a, const cv::Rect& b) {
  if (is_rect_empty(a))
    return b;
  if (is_rect_empty(b))
    return a;
  int x0 = std::min(a.x, b.x);
  int y0 = std::min(a.y, b.y);
  int x1 = std::max(a.x + a.width, b.x + b.width);
  int y1 = std::max(a.y + a.height, b.y + b.height);
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

cv::Rect clamp_rect_to_bounds(const cv::Rect& rect, const cv::Size& bounds) {
  if (bounds.width <= 0 || bounds.height <= 0 || is_rect_empty(rect))
    return cv::Rect();
  int x0 = std::clamp(rect.x, 0, bounds.width);
  int y0 = std::clamp(rect.y, 0, bounds.height);
  int x1 = std::clamp(rect.x + rect.width, 0, bounds.width);
  int y1 = std::clamp(rect.y + rect.height, 0, bounds.height);
  if (x1 <= x0 || y1 <= y0)
    return cv::Rect();
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

cv::Point2d apply_matrix(const std::array<double, 9>& mat, double x, double y) {
  double w = mat[6] * x + mat[7] * y + mat[8];
  if (std::abs(w) < 1e-9)
    w = 1.0;
  double inv_w = 1.0 / w;
  double tx = (mat[0] * x + mat[1] * y + mat[2]) * inv_w;
  double ty = (mat[3] * x + mat[4] * y + mat[5]) * inv_w;
  return {tx, ty};
}

cv::Rect transform_rect_with_matrix(const cv::Rect& rect,
                                    const std::array<double, 9>& mat) {
  if (is_rect_empty(rect))
    return cv::Rect();
  std::array<cv::Point2d, 4> pts{
      apply_matrix(mat, rect.x, rect.y),
      apply_matrix(mat, rect.x + rect.width, rect.y),
      apply_matrix(mat, rect.x, rect.y + rect.height),
      apply_matrix(mat, rect.x + rect.width, rect.y + rect.height)};
  double min_x = pts[0].x;
  double max_x = pts[0].x;
  double min_y = pts[0].y;
  double max_y = pts[0].y;
  for (size_t i = 1; i < pts.size(); ++i) {
    min_x = std::min(min_x, pts[i].x);
    max_x = std::max(max_x, pts[i].x);
    min_y = std::min(min_y, pts[i].y);
    max_y = std::max(max_y, pts[i].y);
  }
  int x0 = static_cast<int>(std::floor(min_x));
  int y0 = static_cast<int>(std::floor(min_y));
  int x1 = static_cast<int>(std::ceil(max_x));
  int y1 = static_cast<int>(std::ceil(max_y));
  if (x1 <= x0 || y1 <= y0)
    return cv::Rect();
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

const NodeOutput* pick_cached_output(const Node& node) {
  if (node.cached_output_high_precision)
    return &node.cached_output_high_precision.value();
  return nullptr;
}

/**
 * @brief Accumulates alternative upstream ROI contributions for one node.
 *
 * RoiAccumulator keeps the "no ROI yet" state separate from an empty cv::Rect
 * so operator propagation, spatial metadata, and dependency LUT lookup can be
 * merged without losing the distinction between "no contribution" and "empty
 * contribution".
 *
 * @note The accumulator performs bounding-box union only. Callers remain
 * responsible for clipping the final ROI to the appropriate node extent.
 */
class RoiAccumulator {
 public:
  /**
   * @brief Adds one non-empty ROI contribution.
   *
   * @param roi Candidate ROI contribution.
   * @throws Nothing.
   * @note Empty ROIs are ignored to preserve existing fallback behavior.
   */
  void include(const cv::Rect& roi) {
    if (is_rect_empty(roi))
      return;
    roi_ = has_roi_ ? merge_rect(roi_, roi) : roi;
    has_roi_ = true;
  }

  /**
   * @brief Returns the accumulated ROI.
   *
   * @return Bounding-box union of included ROIs, or an empty rect when no ROI
   * was included.
   * @throws Nothing.
   */
  cv::Rect value() const { return has_roi_ ? roi_ : cv::Rect(); }

 private:
  cv::Rect roi_;
  bool has_roi_ = false;
};

/**
 * @brief Owns graph-level ROI frontier traversal state.
 *
 * RoiFrontier stores the best known ROI per node plus the pending queue used by
 * graph-level forward/backward projection. It hides the repeated "merge ROI,
 * clip to node bounds, enqueue when changed" sequence shared by both
 * projection directions.
 *
 * @note The frontier stores node ids and value-type cv::Rect records only; it
 * never stores GraphModel pointers or scheduler/runtime state.
 */
class RoiFrontier {
 public:
  /**
   * @brief Seeds the traversal with a clipped ROI.
   *
   * @param node_id Starting node id.
   * @param roi Candidate starting ROI.
   * @param bounds Output extent used to clip the ROI.
   * @return True when a non-empty seed was accepted.
   * @throws std::bad_alloc if map or queue storage grows and allocation fails.
   */
  bool seed(int node_id, const cv::Rect& roi, const cv::Size& bounds) {
    const cv::Rect clipped = clamp_rect_to_bounds(roi, bounds);
    if (is_rect_empty(clipped))
      return false;
    roi_map_[node_id] = clipped;
    pending_.push(node_id);
    return true;
  }

  /**
   * @brief Checks whether traversal has pending nodes.
   *
   * @return True when no frontier node remains queued.
   * @throws Nothing.
   */
  bool empty() const { return pending_.empty(); }

  /**
   * @brief Removes and returns the next queued node id.
   *
   * @return Node id at the front of the traversal queue.
   * @throws Undefined behavior if called when empty() is true.
   */
  int pop() {
    const int node_id = pending_.front();
    pending_.pop();
    return node_id;
  }

  /**
   * @brief Reads the current ROI for one node and clips it to bounds.
   *
   * @param node_id Node id to read.
   * @param bounds Output extent used to clip the ROI.
   * @return Clipped ROI when present and non-empty; otherwise nullopt.
   * @throws Nothing.
   */
  std::optional<cv::Rect> clipped_roi(int node_id,
                                      const cv::Size& bounds) const {
    const auto it = roi_map_.find(node_id);
    if (it == roi_map_.end())
      return std::nullopt;
    const cv::Rect clipped = clamp_rect_to_bounds(it->second, bounds);
    if (is_rect_empty(clipped))
      return std::nullopt;
    return clipped;
  }

  /**
   * @brief Merges a propagated ROI into a node and enqueues it when changed.
   *
   * @param node_id Node receiving propagated ROI.
   * @param roi Candidate propagated ROI.
   * @param bounds Output extent used to clip the merged ROI.
   * @return True when node_id was newly inserted or its ROI changed.
   * @throws std::bad_alloc if map or queue storage grows and allocation fails.
   * @note The method preserves prior behavior where only changed bounding boxes
   * are requeued.
   */
  bool merge_or_enqueue(int node_id, const cv::Rect& roi,
                        const cv::Size& bounds) {
    const cv::Rect clipped = clamp_rect_to_bounds(roi, bounds);
    if (is_rect_empty(clipped))
      return false;

    const auto it = roi_map_.find(node_id);
    if (it == roi_map_.end()) {
      roi_map_[node_id] = clipped;
      pending_.push(node_id);
      return true;
    }

    const cv::Rect merged =
        clamp_rect_to_bounds(merge_rect(it->second, clipped), bounds);
    if (merged == it->second || is_rect_empty(merged))
      return false;
    roi_map_[node_id] = merged;
    pending_.push(node_id);
    return true;
  }

 private:
  std::unordered_map<int, cv::Rect> roi_map_;
  std::queue<int> pending_;
};

SpatialDependencyMap& normalize_dependency_map(SpatialDependencyMap& map,
                                               const cv::Size& child_size) {
  if (map.grid_size_x <= 0)
    map.grid_size_x = 64;
  if (map.grid_size_y <= 0)
    map.grid_size_y = 64;
  if (map.output_extent.width <= 0 || map.output_extent.height <= 0 ||
      map.output_extent != child_size)
    map.output_extent = child_size;
  if (map.cols <= 0)
    map.cols =
        (map.output_extent.width + map.grid_size_x - 1) / map.grid_size_x;
  if (map.rows <= 0)
    map.rows =
        (map.output_extent.height + map.grid_size_y - 1) / map.grid_size_y;
  int required = map.cols * map.rows;
  if (required > 0 &&
      static_cast<int>(map.cell_to_upstream_roi.size()) < required) {
    map.cell_to_upstream_roi.resize(required);
  }
  return map;
}

cv::Rect dependency_lookup(const Node& node, const GraphModel& graph,
                           const DependencyLutBuilder& builder,
                           const cv::Rect& current_roi,
                           const cv::Size& parent_size,
                           const cv::Size& child_size) {
  if (is_rect_empty(current_roi) || parent_size.width <= 0 ||
      parent_size.height <= 0 || child_size.width <= 0 ||
      child_size.height <= 0) {
    return cv::Rect();
  }
  if (!node.dependency_lut || !node.dependency_lut->is_valid_for(child_size)) {
    SpatialDependencyMap lut = builder(node, graph, parent_size, child_size);
    normalize_dependency_map(lut, child_size);
    node.dependency_lut = std::move(lut);
    node.dependency_lut_version += 1;
  }
  if (!node.dependency_lut || !node.dependency_lut->is_valid_for(child_size)) {
    return cv::Rect();
  }
  return node.dependency_lut->lookup(current_roi);
}

/**
 * @brief Resolves the first valid image-input parent extent for a node.
 *
 * @tparam SizeResolver Callable taking a node id and returning cv::Size.
 * @param graph Graph providing image-input edges.
 * @param node_id Child node whose primary parent is requested.
 * @param get_size Extent resolver with request-local caching.
 * @return First valid image-input parent extent, or an empty size.
 * @throws GraphError from get_size when extent resolution fails.
 * @note Data-dependent LUT builders currently consume one primary image-input
 * extent, matching the legacy propagation path.
 */
template <typename SizeResolver>
cv::Size first_valid_image_parent_size(const GraphModel& graph, int node_id,
                                       SizeResolver get_size) {
  for (const auto& edge : graph.upstream_edges(node_id)) {
    if (edge.kind != GraphTopologyEdgeKind::ImageInput ||
        edge.from_node_id < 0 || !graph.has_node(edge.from_node_id)) {
      continue;
    }
    cv::Size parent_size = get_size(edge.from_node_id);
    if (parent_size.width > 0 && parent_size.height > 0)
      return parent_size;
  }
  return cv::Size();
}

/**
 * @brief Adds operator-provided dirty propagation to an accumulator.
 *
 * @param node Node whose operator propagator is resolved.
 * @param clamped_roi Downstream ROI already clipped to node output extent.
 * @param graph Graph passed to the operator propagator.
 * @param accumulator Accumulator receiving the propagated upstream ROI.
 * @throws Exceptions propagated by registered operator propagators.
 */
void append_operator_upstream_roi(const Node& node, const cv::Rect& clamped_roi,
                                  const GraphModel& graph,
                                  RoiAccumulator& accumulator) {
  auto propagate_fn =
      OpRegistry::instance().get_dirty_propagator(node.type, node.subtype);
  accumulator.include(propagate_fn(node, clamped_roi, graph));
}

/**
 * @brief Adds cached spatial metadata projection to an accumulator.
 *
 * @param node Node whose single-input spatial metadata may be used.
 * @param clamped_roi Downstream ROI already clipped to node output extent.
 * @param accumulator Accumulator receiving the matrix-projected ROI.
 * @throws Nothing directly.
 * @note Spatial metadata contributes only for single-image-input nodes with a
 * high-precision cached output, preserving the existing HP-authoritative rule.
 */
void append_spatial_metadata_roi(const Node& node, const cv::Rect& clamped_roi,
                                 RoiAccumulator& accumulator) {
  if (node.image_inputs.size() != 1)
    return;
  if (const NodeOutput* cached = pick_cached_output(node)) {
    accumulator.include(transform_rect_with_matrix(
        clamped_roi, cached->space.local_inverse_matrix));
  }
}

/**
 * @brief Adds data-dependent dependency LUT projection to an accumulator.
 *
 * @tparam SizeResolver Callable taking a node id and returning cv::Size.
 * @param node Node whose dependency builder may be registered.
 * @param graph Graph used by the dependency LUT builder.
 * @param clamped_roi Downstream ROI already clipped to node output extent.
 * @param get_size Extent resolver with request-local caching.
 * @param accumulator Accumulator receiving LUT-derived upstream ROI.
 * @throws Exceptions propagated by the registered LUT builder or extent
 * resolver.
 * @note The LUT contribution is merged with static/operator propagation rather
 * than replacing it.
 */
template <typename SizeResolver>
void append_dependency_lut_roi(const Node& node, const GraphModel& graph,
                               const cv::Rect& clamped_roi,
                               SizeResolver get_size,
                               RoiAccumulator& accumulator) {
  const auto lut_builder =
      OpRegistry::instance().get_dependency_builder(node.type, node.subtype);
  if (!lut_builder)
    return;

  const cv::Size downstream_size = get_size(node.id);
  const cv::Size primary_parent_size =
      first_valid_image_parent_size(graph, node.id, get_size);
  if (primary_parent_size.width <= 0 || primary_parent_size.height <= 0 ||
      downstream_size.width <= 0 || downstream_size.height <= 0) {
    return;
  }
  accumulator.include(dependency_lookup(node, graph, *lut_builder, clamped_roi,
                                        primary_parent_size, downstream_size));
}

/**
 * @brief Propagates one forward image edge into a child ROI.
 *
 * @param graph Graph containing the child node.
 * @param edge Image-input edge from the current parent to a child.
 * @param parent_roi Current ROI in parent output coordinates.
 * @param parent_size Parent output extent.
 * @param child_size Child output extent.
 * @return Child ROI clipped to child output extent, or nullopt when no work is
 * affected.
 * @throws Exceptions propagated by registered forward propagators.
 */
std::optional<cv::Rect> propagate_forward_edge_roi(
    const GraphModel& graph, const GraphTopologyEdge& edge,
    const cv::Rect& parent_roi, const cv::Size& parent_size,
    const cv::Size& child_size) {
  const Node& child = graph.node(edge.to_node_id);
  auto forward_fn =
      OpRegistry::instance().get_forward_propagator(child.type, child.subtype);
  const cv::Rect propagated = clamp_rect_to_bounds(
      forward_fn(child, parent_roi, graph, parent_size, child_size),
      child_size);
  if (is_rect_empty(propagated))
    return std::nullopt;
  return propagated;
}

/**
 * @brief Enqueues parent ROIs produced by a backward propagation step.
 *
 * @tparam SizeResolver Callable taking a node id and returning cv::Size.
 * @param graph Graph containing the current node's image-input edges.
 * @param current_id Node whose parents receive upstream demand.
 * @param upstream_roi Upstream ROI computed by RoiPropagationService.
 * @param get_size Extent resolver with request-local caching.
 * @param frontier Frontier receiving parent ROI merges.
 * @throws GraphError from get_size when extent resolution fails.
 * @note The same upstream ROI is clipped independently to each valid image
 * parent, matching the previous multi-input propagation behavior.
 */
template <typename SizeResolver>
void enqueue_backward_parent_rois(const GraphModel& graph, int current_id,
                                  const cv::Rect& upstream_roi,
                                  SizeResolver get_size,
                                  RoiFrontier& frontier) {
  for (const auto& edge : graph.upstream_edges(current_id)) {
    if (edge.kind != GraphTopologyEdgeKind::ImageInput ||
        edge.from_node_id < 0 || !graph.has_node(edge.from_node_id)) {
      continue;
    }
    const int parent_id = edge.from_node_id;
    frontier.merge_or_enqueue(parent_id, upstream_roi, get_size(parent_id));
  }
}

}  // namespace

cv::Rect RoiPropagationService::compute_upstream_roi(
    const Node& node, const cv::Rect& downstream_roi, const GraphModel& graph,
    std::unordered_map<int, cv::Size>& size_cache) const {
  if (is_rect_empty(downstream_roi))
    return cv::Rect();

  auto get_size = [&](int nid) {
    return extent_resolver_.resolve_output_extent(graph, nid, size_cache);
  };
  cv::Rect clamped_roi =
      clamp_rect_to_bounds(downstream_roi, get_size(node.id));
  if (is_rect_empty(clamped_roi))
    return cv::Rect();

  RoiAccumulator upstream;
  append_operator_upstream_roi(node, clamped_roi, graph, upstream);
  append_spatial_metadata_roi(node, clamped_roi, upstream);
  append_dependency_lut_roi(node, graph, clamped_roi, get_size, upstream);
  return upstream.value();
}

std::optional<cv::Rect> RoiPropagationService::project_roi_forward(
    const GraphModel& graph, int start_node_id, const cv::Rect& start_roi,
    int target_node_id) const {
  if (!graph.has_node(start_node_id) || !graph.has_node(target_node_id))
    return std::nullopt;
  if (is_rect_empty(start_roi))
    return std::nullopt;

  std::unordered_map<int, cv::Size> size_cache;
  auto get_size = [&](int nid) {
    return extent_resolver_.resolve_output_extent(graph, nid, size_cache);
  };

  RoiFrontier frontier;

  if (!frontier.seed(start_node_id, start_roi, get_size(start_node_id)))
    return std::nullopt;

  while (!frontier.empty()) {
    int current = frontier.pop();
    auto current_roi = frontier.clipped_roi(current, get_size(current));
    if (!current_roi)
      continue;
    if (current == target_node_id)
      break;

    cv::Size parent_size = get_size(current);
    for (const auto& edge : graph.downstream_edges(current)) {
      if (edge.kind != GraphTopologyEdgeKind::ImageInput) {
        continue;
      }
      int child_id = edge.to_node_id;
      cv::Size child_size = get_size(child_id);
      if (child_size.width <= 0 || child_size.height <= 0)
        continue;

      auto propagated = propagate_forward_edge_roi(graph, edge, *current_roi,
                                                   parent_size, child_size);
      if (propagated) {
        frontier.merge_or_enqueue(child_id, *propagated, child_size);
      }
    }
  }

  return frontier.clipped_roi(target_node_id, get_size(target_node_id));
}

std::optional<cv::Rect> RoiPropagationService::project_roi_backward(
    const GraphModel& graph, int target_node_id, const cv::Rect& target_roi,
    int source_node_id) const {
  if (!graph.has_node(target_node_id) || !graph.has_node(source_node_id))
    return std::nullopt;
  if (is_rect_empty(target_roi))
    return std::nullopt;

  std::unordered_map<int, cv::Size> size_cache;
  auto get_size = [&](int nid) {
    return extent_resolver_.resolve_output_extent(graph, nid, size_cache);
  };
  RoiFrontier frontier;

  if (!frontier.seed(target_node_id, target_roi, get_size(target_node_id)))
    return std::nullopt;

  while (!frontier.empty()) {
    int current = frontier.pop();
    auto current_roi = frontier.clipped_roi(current, get_size(current));
    if (!current_roi)
      continue;
    if (current == source_node_id)
      return current_roi;

    const Node& node = graph.node(current);

    cv::Rect upstream_roi =
        compute_upstream_roi(node, *current_roi, graph, size_cache);
    if (is_rect_empty(upstream_roi))
      continue;

    enqueue_backward_parent_rois(graph, current, upstream_roi, get_size,
                                 frontier);
  }

  return std::nullopt;
}

}  // namespace ps
