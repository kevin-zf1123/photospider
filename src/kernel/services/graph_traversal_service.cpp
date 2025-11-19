#include "kernel/services/graph_traversal_service.hpp"

#include <yaml-cpp/emitter.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <queue>
#include <unordered_map>

#include "graph_model.hpp"
#include "kernel/ops.hpp"
#include "kernel/param_utils.hpp"

namespace ps {

namespace {
constexpr int kRtDownscaleFactor = 4;

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

cv::Rect expand_rect(const cv::Rect& rect, int padding) {
  if (padding <= 0 || is_rect_empty(rect))
    return rect;
  return cv::Rect(rect.x - padding, rect.y - padding,
                  rect.width + padding * 2, rect.height + padding * 2);
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

cv::Point2d apply_matrix(const std::array<double, 9>& mat, double x,
                         double y) {
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
  if (node.cached_output)
    return &node.cached_output.value();
  if (node.cached_output_high_precision)
    return &node.cached_output_high_precision.value();
  if (node.cached_output_real_time)
    return &node.cached_output_real_time.value();
  return nullptr;
}

void topo_postorder_util(const GraphModel& graph, int node_id,
                         std::vector<int>& order,
                         std::unordered_map<int, bool>& visited,
                         std::unordered_map<int, bool>& recursion_stack) {
  visited[node_id] = true;
  recursion_stack[node_id] = true;

  const auto& node = graph.nodes.at(node_id);
  auto process_dependency = [&](int dependency_id) {
    if (dependency_id == -1 || !graph.has_node(dependency_id)) {
      return;
    }
    if (!visited[dependency_id]) {
      topo_postorder_util(graph, dependency_id, order, visited,
                          recursion_stack);
      return;
    }
    if (recursion_stack[dependency_id]) {
      throw GraphError(GraphErrc::Cycle,
                       "Cycle detected in graph during traversal involving " +
                           std::to_string(dependency_id));
    }
  };

  for (const auto& input : node.image_inputs) {
    process_dependency(input.from_node_id);
  }
  for (const auto& input : node.parameter_inputs) {
    process_dependency(input.from_node_id);
  }

  order.push_back(node_id);
  recursion_stack[node_id] = false;
}

void print_dep_tree_recursive(const GraphModel& graph, std::ostream& os,
                              int node_id, int level,
                              std::unordered_set<int>& path,
                              bool show_parameters) {
  auto indent = [&](int l) {
    for (int i = 0; i < l; ++i) {
      os << "  ";
    }
  };

  os << "\n";

  if (path.count(node_id)) {
    indent(level);
    os << "- ... (Cycle detected on Node " << node_id << ") ...\n";
    return;
  }
  path.insert(node_id);

  indent(level);
  const auto& node = graph.nodes.at(node_id);
  os << "- Node " << node.id << " (" << node.name << " | " << node.type << ":"
     << node.subtype << ")\n";

  if (show_parameters && node.parameters && node.parameters.IsMap() &&
      node.parameters.size() > 0) {
    indent(level + 1);
    os << "static_params:\n";

    std::function<void(const YAML::Node&, int)> dump_map =
        [&](const YAML::Node& m, int lvl) {
          for (auto it = m.begin(); it != m.end(); ++it) {
            indent(lvl);
            std::string key;
            YAML::Node key_node = it->first;
            try {
              key = key_node.as<std::string>();
            } catch (...) {
              YAML::Emitter ke;
              ke << it->first;
              key = ke.c_str();
            }
            YAML::Node val = it->second;
            if (val.IsMap()) {
              os << key << ":\n";
              dump_map(val, lvl + 1);
            } else {
              os << key << ": ";
              YAML::Emitter ve;
              ve << YAML::Flow << val;
              os << ve.c_str() << "\n";
            }
          }
        };

    for (auto it = node.parameters.begin(); it != node.parameters.end(); ++it) {
      YAML::Node key_node = it->first;
      YAML::Node val_node = it->second;
      std::string key;
      try {
        key = key_node.as<std::string>();
      } catch (...) {
        YAML::Emitter ke;
        ke << key_node;
        key = ke.c_str();
      }

      if (val_node.IsMap()) {
        indent(level + 2);
        os << key << ":\n";
        dump_map(val_node, level + 3);
      } else {
        indent(level + 2);
        os << key << ": ";
        YAML::Emitter ve;
        ve << YAML::Flow << val_node;
        os << ve.c_str() << "\n";
      }
    }
  }

  for (const auto& input : node.image_inputs) {
    if (input.from_node_id != -1 && graph.has_node(input.from_node_id)) {
      os << "\n";
      indent(level + 1);
      os << "(image from " << input.from_node_id << ":"
         << input.from_output_name << ")\n";
      print_dep_tree_recursive(graph, os, input.from_node_id, level + 2, path,
                               show_parameters);
    }
  }
  for (const auto& input : node.parameter_inputs) {
    if (input.from_node_id != -1 && graph.has_node(input.from_node_id)) {
      os << "\n";
      indent(level + 1);
      os << "(param '" << input.to_parameter_name << "' from "
         << input.from_node_id << ":" << input.from_output_name << ")\n";
      print_dep_tree_recursive(graph, os, input.from_node_id, level + 2, path,
                               show_parameters);
    }
  }
  path.erase(node_id);
}

}  // namespace

std::vector<int> GraphTraversalService::topo_postorder_from(
    const GraphModel& graph, int end_node_id) const {
  if (!graph.has_node(end_node_id)) {
    throw GraphError(GraphErrc::NotFound,
                     "Node " + std::to_string(end_node_id) + " not in graph.");
  }

  std::vector<int> order;
  std::unordered_map<int, bool> visited;
  std::unordered_map<int, bool> recursion_stack;

  topo_postorder_util(graph, end_node_id, order, visited, recursion_stack);
  return order;
}

bool GraphTraversalService::is_ancestor(
    const GraphModel& graph, int potential_ancestor_id, int node_id,
    std::unordered_set<int>& visited) const {
  if (potential_ancestor_id == node_id) {
    return true;
  }

  if (visited.count(node_id)) {
    return false;
  }
  visited.insert(node_id);

  if (!graph.has_node(node_id)) {
    return false;
  }

  const auto& node = graph.nodes.at(node_id);
  for (const auto& input : node.image_inputs) {
    if (input.from_node_id != -1 && is_ancestor(graph, potential_ancestor_id,
                                                input.from_node_id, visited)) {
      return true;
    }
  }
  for (const auto& input : node.parameter_inputs) {
    if (input.from_node_id != -1 && is_ancestor(graph, potential_ancestor_id,
                                                input.from_node_id, visited)) {
      return true;
    }
  }
  return false;
}

std::vector<int> GraphTraversalService::ending_nodes(
    const GraphModel& graph) const {
  std::unordered_set<int> is_input_to_something;
  for (const auto& pair : graph.nodes) {
    for (const auto& input : pair.second.image_inputs) {
      is_input_to_something.insert(input.from_node_id);
    }
    for (const auto& input : pair.second.parameter_inputs) {
      is_input_to_something.insert(input.from_node_id);
    }
  }
  std::vector<int> ends;
  ends.reserve(graph.nodes.size());
  for (const auto& pair : graph.nodes) {
    if (is_input_to_something.find(pair.first) == is_input_to_something.end()) {
      ends.push_back(pair.first);
    }
  }
  return ends;
}

std::vector<int> GraphTraversalService::parents_of(const GraphModel& graph,
                                                   int node_id) const {
  std::vector<int> parents;
  parents.reserve(graph.nodes.size());

  for (const auto& pair : graph.nodes) {
    const auto& candidate = pair.second;
    for (const auto& input : candidate.image_inputs) {
      if (input.from_node_id == node_id) {
        parents.push_back(candidate.id);
        break;
      }
    }
    for (const auto& input : candidate.parameter_inputs) {
      if (input.from_node_id == node_id) {
        parents.push_back(candidate.id);
        break;
      }
    }
  }
  return parents;
}

std::vector<int> GraphTraversalService::get_trees_containing_node(
    const GraphModel& graph, int node_id) const {
  std::vector<int> result_trees;
  auto all_end_nodes = ending_nodes(graph);
  for (int end_node : all_end_nodes) {
    try {
      auto order = topo_postorder_from(graph, end_node);
      if (std::find(order.begin(), order.end(), node_id) != order.end()) {
        result_trees.push_back(end_node);
      }
    } catch (const GraphError&) {
      continue;
    }
  }
  return result_trees;
}

void GraphTraversalService::print_dependency_tree(const GraphModel& graph,
                                                  std::ostream& os,
                                                  bool show_parameters) const {
  os << "Dependency Tree (reversed from ending nodes):\n";
  auto ends = ending_nodes(graph);
  if (ends.empty() && !graph.nodes.empty()) {
    os << "(Graph has cycles or is fully connected)\n";
  } else if (graph.nodes.empty()) {
    os << "(Graph is empty)\n";
  }

  for (int end_node_id : ends) {
    std::unordered_set<int> path;
    print_dep_tree_recursive(graph, os, end_node_id, 0, path, show_parameters);
  }
}

void GraphTraversalService::print_dependency_tree(const GraphModel& graph,
                                                  std::ostream& os,
                                                  int start_node_id,
                                                  bool show_parameters) const {
  os << "Dependency Tree (starting from Node " << start_node_id << "):\n";
  if (!graph.has_node(start_node_id)) {
    os << "(Node " << start_node_id << " not found in graph)\n";
    return;
  }
  std::unordered_set<int> path;
  print_dep_tree_recursive(graph, os, start_node_id, 0, path, show_parameters);
}

namespace {

std::optional<int> node_param_int_opt(const Node& node,
                                      const std::string& key) {
  if (node.runtime_parameters && node.runtime_parameters[key]) {
    try {
      return node.runtime_parameters[key].as<int>();
    } catch (...) {
    }
  }
  if (node.parameters && node.parameters[key]) {
    try {
      return node.parameters[key].as<int>();
    } catch (...) {
    }
  }
  return std::nullopt;
}

std::optional<double> node_param_double_opt(const Node& node,
                                            const std::string& key) {
  if (node.runtime_parameters && node.runtime_parameters[key]) {
    try {
      return node.runtime_parameters[key].as<double>();
    } catch (...) {
    }
  }
  if (node.parameters && node.parameters[key]) {
    try {
      return node.parameters[key].as<double>();
    } catch (...) {
    }
  }
  return std::nullopt;
}

std::optional<std::string> node_param_string_opt(const Node& node,
                                                 const std::string& key) {
  if (node.runtime_parameters && node.runtime_parameters[key]) {
    try {
      return node.runtime_parameters[key].as<std::string>();
    } catch (...) {
    }
  }
  if (node.parameters && node.parameters[key]) {
    try {
      return node.parameters[key].as<std::string>();
    } catch (...) {
    }
  }
  return std::nullopt;
}

cv::Size infer_output_size(const GraphModel& graph,
                           std::unordered_map<int, cv::Size>& cache,
                           int node_id) {
  auto cached = cache.find(node_id);
  if (cached != cache.end())
    return cached->second;

  cv::Size size{0, 0};
  const Node& node = graph.nodes.at(node_id);

  auto take_from_output = [&](const std::optional<NodeOutput>& opt) -> bool {
    if (!opt)
      return false;
    const auto& buf = opt->image_buffer;
    if (buf.width <= 0 || buf.height <= 0)
      return false;
    size = cv::Size(buf.width, buf.height);
    return true;
  };
  if (take_from_output(node.cached_output_high_precision))
    return cache[node_id] = size;
  if (take_from_output(node.cached_output))
    return cache[node_id] = size;
  if (node.cached_output_real_time) {
    const auto& buf = node.cached_output_real_time->image_buffer;
    if (buf.width > 0 && buf.height > 0) {
      size = cv::Size(buf.width * kRtDownscaleFactor,
                      buf.height * kRtDownscaleFactor);
      return cache[node_id] = size;
    }
  }

  auto width_opt = node_param_int_opt(node, "width");
  auto height_opt = node_param_int_opt(node, "height");
  if (width_opt && height_opt && *width_opt > 0 && *height_opt > 0) {
    size = cv::Size(*width_opt, *height_opt);
    return cache[node_id] = size;
  }

  for (const auto& input : node.image_inputs) {
    if (input.from_node_id < 0)
      continue;
    cv::Size parent_size = infer_output_size(graph, cache, input.from_node_id);
    if (parent_size.width > 0 && parent_size.height > 0) {
      size = parent_size;
      break;
    }
  }

  return cache[node_id] = size;
}

int infer_neighborhood_radius(const Node& node) {
  int radius = 0;
  auto try_update = [&](std::optional<int> candidate) {
    if (candidate)
      radius = std::max(radius, std::max(0, *candidate));
  };
  try_update(node_param_int_opt(node, "radius"));
  try_update(node_param_int_opt(node, "kernel_radius"));
  try_update(node_param_int_opt(node, "border"));
  if (auto ksize = node_param_int_opt(node, "ksize")) {
    if (*ksize > 0)
      radius = std::max(radius, std::max(0, (*ksize - 1) / 2));
  }
  if (auto kernel_size = node_param_int_opt(node, "kernel_size")) {
    if (*kernel_size > 0)
      radius = std::max(radius, std::max(0, (*kernel_size - 1) / 2));
  }
  return radius;
}

}  // namespace

std::optional<cv::Rect> GraphTraversalService::project_roi_forward(
    const GraphModel& graph, int start_node_id, const cv::Rect& start_roi,
    int target_node_id) const {
  if (!graph.has_node(start_node_id) || !graph.has_node(target_node_id))
    return std::nullopt;
  if (is_rect_empty(start_roi))
    return std::nullopt;

  std::unordered_map<int, std::vector<std::pair<int, int>>> child_map;
  for (const auto& [child_id, node] : graph.nodes) {
    for (size_t idx = 0; idx < node.image_inputs.size(); ++idx) {
      int parent_id = node.image_inputs[idx].from_node_id;
      if (parent_id < 0)
        continue;
      child_map[parent_id].emplace_back(child_id, static_cast<int>(idx));
    }
  }

  std::unordered_map<int, cv::Size> size_cache;
  auto get_size = [&](int nid) { return infer_output_size(graph, size_cache, nid); };

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
    const cv::Rect current_roi = roi_map[current];
    if (is_rect_empty(current_roi))
      continue;
    if (current == target_node_id)
      break;

    auto child_it = child_map.find(current);
    if (child_it == child_map.end())
      continue;

    for (const auto& link : child_it->second) {
      int child_id = link.first;
      int input_index = link.second;
      const Node& child = graph.nodes.at(child_id);
      cv::Rect propagated;

      auto clamp_to_child = [&](const cv::Rect& rect) {
        return clamp_rect_to_bounds(rect, get_size(child_id));
      };

      if (child.type == "image_process") {
        if (child.subtype == "gaussian_blur" ||
            child.subtype == "gaussian_blur_tiled" ||
            child.subtype == "convolve") {
          int radius = infer_neighborhood_radius(child);
          if (radius <= 0)
            radius = 1;
          propagated = clamp_to_child(expand_rect(current_roi, radius));
        } else if (child.subtype == "crop") {
          cv::Size parent_size = get_size(current);
          cv::Rect crop_rect;
          std::string mode =
              node_param_string_opt(child, "mode").value_or("value");
          if (mode == "ratio") {
            if (parent_size.width <= 0 || parent_size.height <= 0)
              continue;
            auto rx = node_param_double_opt(child, "x");
            auto ry = node_param_double_opt(child, "y");
            auto rw = node_param_double_opt(child, "width");
            auto rh = node_param_double_opt(child, "height");
            if (!rx || !ry || !rw || !rh)
              continue;
            int x = static_cast<int>(*rx * parent_size.width);
            int y = static_cast<int>(*ry * parent_size.height);
            int w = static_cast<int>(*rw * parent_size.width);
            int h = static_cast<int>(*rh * parent_size.height);
            if (w <= 0 || h <= 0)
              continue;
            crop_rect = cv::Rect(x, y, w, h);
          } else {
            int x = node_param_int_opt(child, "x").value_or(0);
            int y = node_param_int_opt(child, "y").value_or(0);
            auto w = node_param_int_opt(child, "width");
            auto h = node_param_int_opt(child, "height");
            if (!w || !h || *w <= 0 || *h <= 0)
              continue;
            crop_rect = cv::Rect(x, y, *w, *h);
          }
          cv::Rect intersect = current_roi & crop_rect;
          if (is_rect_empty(intersect))
            continue;
          cv::Rect child_roi(intersect.x - crop_rect.x, intersect.y - crop_rect.y,
                             intersect.width, intersect.height);
          propagated = clamp_to_child(child_roi);
        } else if (child.subtype == "resize") {
          cv::Size parent_size = get_size(current);
          cv::Size child_size = get_size(child_id);
          if (parent_size.width <= 0 || parent_size.height <= 0 ||
              child_size.width <= 0 || child_size.height <= 0)
            continue;

          double scale_x =
              static_cast<double>(child_size.width) / parent_size.width;
          double scale_y =
              static_cast<double>(child_size.height) / parent_size.height;

          double left = static_cast<double>(current_roi.x) * scale_x;
          double top = static_cast<double>(current_roi.y) * scale_y;
          double right =
              static_cast<double>(current_roi.x + current_roi.width) * scale_x;
          double bottom =
              static_cast<double>(current_roi.y + current_roi.height) * scale_y;

          int x0 = static_cast<int>(std::floor(left)) - 2;
          int y0 = static_cast<int>(std::floor(top)) - 2;
          int x1 = static_cast<int>(std::ceil(right)) + 2;
          int y1 = static_cast<int>(std::ceil(bottom)) + 2;

          propagated = clamp_to_child(cv::Rect(x0, y0, x1 - x0, y1 - y0));
        } else if (child.subtype == "curve_transform") {
          propagated = clamp_to_child(current_roi);
        }
      }

      if (child.type == "image_mixing" && child.subtype == "add_weighted") {
        (void)input_index;
        propagated = clamp_to_child(current_roi);
      }

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

std::optional<cv::Rect> GraphTraversalService::project_roi_backward(
    const GraphModel& graph, int target_node_id, const cv::Rect& target_roi,
    int source_node_id) const {
  if (!graph.has_node(target_node_id) || !graph.has_node(source_node_id))
    return std::nullopt;
  if (is_rect_empty(target_roi))
    return std::nullopt;

  std::unordered_map<int, cv::Rect> roi_map;
  std::queue<int> pending;
  std::unordered_map<int, cv::Size> size_cache;
  auto get_size = [&](int nid) { return infer_output_size(graph, size_cache, nid); };

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

    const Node& node = graph.nodes.at(current);
    cv::Rect upstream_roi;
    bool used_spatial = false;
    if (node.image_inputs.size() == 1) {
      if (const NodeOutput* cached = pick_cached_output(node)) {
        upstream_roi = transform_rect_with_matrix(
            current_roi, cached->space.local_inverse_matrix);
        if (!is_rect_empty(upstream_roi)) {
          used_spatial = true;
        }
      }
    }
    if (!used_spatial) {
      auto propagate_fn = OpRegistry::instance().get_dirty_propagator(
          node.type, node.subtype);
      upstream_roi = propagate_fn(node, current_roi);
    }

    for (const auto& input : node.image_inputs) {
      int parent_id = input.from_node_id;
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
