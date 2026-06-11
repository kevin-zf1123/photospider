#include "kernel/services/graph_inspect_service.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <vector>

namespace ps {

namespace {
const NodeOutput* pick_cached_output(const Node& node,
                                     std::string& source_label) {
  if (node.cached_output_high_precision) {
    source_label = "HP formal cache";
    return &node.cached_output_high_precision.value();
  }
  if (node.cached_output_real_time) {
    source_label = "RT transient state (NOT formal cache)";
    return &node.cached_output_real_time.value();
  }
  source_label.clear();
  return nullptr;
}
}  // namespace

std::string GraphInspectService::format_node_metadata(
    const Node& node) const {
  std::ostringstream ss;
  std::string source_label;
  const NodeOutput* cached = pick_cached_output(node, source_label);
  if (!cached) {
    ss << "  (No cached output available)\n";
    return ss.str();
  }

  ss << "  [Source] " << source_label << "\n";

  const auto& meta = cached->debug;
  const auto& space = cached->space;

  ss << "  [Debug]\n";
  ss << "    Worker ID: " << meta.computed_by_worker_id << "\n";
  ss << "    Device:    " << meta.compute_device << "\n";
  ss << "    Timestamp: " << meta.timestamp_us << " us\n";
  ss << "    Exec(ms):  " << meta.execution_time_ms << "\n";
  ss << "    Range:     [" << meta.min_val << ", " << meta.max_val << "]";
  if (meta.has_nan) {
    ss << " (NaN detected)";
  }
  ss << "\n";

  ss << "  [Spatial]\n";
  ss << "    Scale:     " << space.global_scale_x << " x "
     << space.global_scale_y << "\n";
  ss << "    ROI:       (" << space.absolute_roi.x << ", "
     << space.absolute_roi.y << ", " << space.absolute_roi.width << "x"
     << space.absolute_roi.height << ")\n";

  auto print_matrix = [&ss](const char* label,
                            const std::array<double, 9>& mat) {
    ss << "    " << label << ":\n";
    ss << "      [" << mat[0] << ", " << mat[1] << ", " << mat[2] << "]\n";
    ss << "      [" << mat[3] << ", " << mat[4] << ", " << mat[5] << "]\n";
    ss << "      [" << mat[6] << ", " << mat[7] << ", " << mat[8] << "]\n";
  };

  print_matrix("Transform", space.transform_matrix);
  print_matrix("Inverse (Global)", space.inverse_matrix);
  print_matrix("Inverse (Local)", space.local_inverse_matrix);

  return ss.str();
}

std::string GraphInspectService::inspect_all_nodes(
    const GraphModel& graph) const {
  std::ostringstream ss;
  std::vector<int> ids;
  ids.reserve(graph.nodes.size());
  for (const auto& kv : graph.nodes) {
    ids.push_back(kv.first);
  }
  std::sort(ids.begin(), ids.end());

  for (size_t idx = 0; idx < ids.size(); ++idx) {
    int id = ids[idx];
    const auto& node = graph.nodes.at(id);
    ss << "=== Node " << node.id << " (" << node.name << " | " << node.type
       << ":" << node.subtype << ") ===\n";
    ss << format_node_metadata(node);
    if (idx + 1 < ids.size()) {
      ss << "\n";
    }
  }
  return ss.str();
}

}  // namespace ps
