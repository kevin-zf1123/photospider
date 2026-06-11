#include "kernel/services/roi_propagation_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <queue>

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

  cv::Rect upstream_roi;
  bool has_roi = false;

  auto propagate_fn =
      OpRegistry::instance().get_dirty_propagator(node.type, node.subtype);
  upstream_roi = propagate_fn(node, clamped_roi, graph);
  if (!is_rect_empty(upstream_roi))
    has_roi = true;

  if (node.image_inputs.size() == 1) {
    if (const NodeOutput* cached = pick_cached_output(node)) {
      cv::Rect spatial_roi = transform_rect_with_matrix(
          clamped_roi, cached->space.local_inverse_matrix);
      if (!is_rect_empty(spatial_roi)) {
        upstream_roi =
            has_roi ? merge_rect(upstream_roi, spatial_roi) : spatial_roi;
        has_roi = true;
      }
    }
  }

  const auto lut_builder =
      OpRegistry::instance().get_dependency_builder(node.type, node.subtype);
  if (lut_builder) {
    cv::Size downstream_size = get_size(node.id);
    cv::Size primary_parent_size;
    for (const auto& edge : graph.upstream_edges(node.id)) {
      if (edge.kind != GraphTopologyEdgeKind::ImageInput ||
          edge.from_node_id < 0 || !graph.has_node(edge.from_node_id)) {
        continue;
      }
      primary_parent_size = get_size(edge.from_node_id);
      if (primary_parent_size.width > 0 && primary_parent_size.height > 0)
        break;
    }
    if (primary_parent_size.width > 0 && primary_parent_size.height > 0 &&
        downstream_size.width > 0 && downstream_size.height > 0) {
      cv::Rect lut_roi =
          dependency_lookup(node, graph, *lut_builder, clamped_roi,
                            primary_parent_size, downstream_size);
      if (!is_rect_empty(lut_roi)) {
        upstream_roi = has_roi ? merge_rect(upstream_roi, lut_roi) : lut_roi;
        has_roi = true;
      }
    }
  }

  return has_roi ? upstream_roi : cv::Rect();
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

  std::unordered_map<int, cv::Rect> roi_map;
  std::queue<int> pending;

  cv::Rect seed_roi = clamp_rect_to_bounds(start_roi, get_size(start_node_id));
  if (is_rect_empty(seed_roi))
    return std::nullopt;

  roi_map[start_node_id] = seed_roi;
  pending.push(start_node_id);

  while (!pending.empty()) {
    int current = pending.front();
    pending.pop();
    cv::Rect current_roi =
        clamp_rect_to_bounds(roi_map[current], get_size(current));
    if (is_rect_empty(current_roi))
      continue;
    if (current == target_node_id)
      break;

    cv::Size parent_size = get_size(current);
    for (const auto& edge : graph.downstream_edges(current)) {
      if (edge.kind != GraphTopologyEdgeKind::ImageInput) {
        continue;
      }
      int child_id = edge.to_node_id;
      const Node& child = graph.node(child_id);
      cv::Size child_size = get_size(child_id);
      if (child_size.width <= 0 || child_size.height <= 0)
        continue;

      auto forward_fn = OpRegistry::instance().get_forward_propagator(
          child.type, child.subtype);
      cv::Rect propagated =
          forward_fn(child, current_roi, graph, parent_size, child_size);
      propagated = clamp_rect_to_bounds(propagated, child_size);

      if (is_rect_empty(propagated))
        continue;

      auto it = roi_map.find(child_id);
      if (it == roi_map.end()) {
        roi_map[child_id] = propagated;
        pending.push(child_id);
      } else {
        cv::Rect merged = merge_rect(it->second, propagated);
        if (merged != it->second) {
          roi_map[child_id] = clamp_rect_to_bounds(merged, get_size(child_id));
          pending.push(child_id);
        }
      }
    }
  }

  auto result_it = roi_map.find(target_node_id);
  if (result_it == roi_map.end())
    return std::nullopt;
  cv::Rect result =
      clamp_rect_to_bounds(result_it->second, get_size(target_node_id));
  if (is_rect_empty(result))
    return std::nullopt;
  return result;
}

std::optional<cv::Rect> RoiPropagationService::project_roi_backward(
    const GraphModel& graph, int target_node_id, const cv::Rect& target_roi,
    int source_node_id) const {
  if (!graph.has_node(target_node_id) || !graph.has_node(source_node_id))
    return std::nullopt;
  if (is_rect_empty(target_roi))
    return std::nullopt;

  std::unordered_map<int, cv::Rect> roi_map;
  std::queue<int> pending;
  std::unordered_map<int, cv::Size> size_cache;
  auto get_size = [&](int nid) {
    return extent_resolver_.resolve_output_extent(graph, nid, size_cache);
  };

  cv::Rect seed = clamp_rect_to_bounds(target_roi, get_size(target_node_id));
  if (is_rect_empty(seed))
    return std::nullopt;
  roi_map[target_node_id] = seed;
  pending.push(target_node_id);

  while (!pending.empty()) {
    int current = pending.front();
    pending.pop();
    cv::Rect current_roi = roi_map[current];
    if (is_rect_empty(current_roi))
      continue;
    if (current == source_node_id)
      return clamp_rect_to_bounds(current_roi, get_size(source_node_id));

    const Node& node = graph.node(current);
    current_roi = clamp_rect_to_bounds(current_roi, get_size(current));
    if (is_rect_empty(current_roi))
      continue;

    cv::Rect upstream_roi =
        compute_upstream_roi(node, current_roi, graph, size_cache);
    if (is_rect_empty(upstream_roi))
      continue;

    for (const auto& edge : graph.upstream_edges(current)) {
      if (edge.kind != GraphTopologyEdgeKind::ImageInput) {
        continue;
      }
      int parent_id = edge.from_node_id;
      if (parent_id < 0 || !graph.has_node(parent_id))
        continue;
      cv::Rect parent_roi =
          clamp_rect_to_bounds(upstream_roi, get_size(parent_id));
      if (is_rect_empty(parent_roi))
        continue;
      auto it = roi_map.find(parent_id);
      if (it == roi_map.end()) {
        roi_map[parent_id] = parent_roi;
        pending.push(parent_id);
      } else {
        cv::Rect merged = merge_rect(it->second, parent_roi);
        if (merged != it->second) {
          roi_map[parent_id] =
              clamp_rect_to_bounds(merged, get_size(parent_id));
          pending.push(parent_id);
        }
      }
    }
  }

  return std::nullopt;
}

}  // namespace ps
