#include "cli/dependency_tree_formatter.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>

namespace ps::cli {

namespace {
void indent(std::ostream& os, int level) {
  for (int i = 0; i < level; ++i) {
    os << "  ";
  }
}

void dump_parameters(std::ostream& os,
                     const std::map<std::string, std::string>& parameters,
                     int level) {
  if (parameters.empty()) {
    return;
  }

  indent(os, level + 1);
  os << "static_params:\n";

  for (const auto& [key, value] : parameters) {
    indent(os, level + 2);
    os << key << ": " << value << "\n";
  }
}

void dump_trimmed_metadata(std::ostream& os, const NodeInspectionView& node,
                           int level) {
  std::istringstream metadata_stream(format_node_metadata(node));
  std::string line;
  while (std::getline(metadata_stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    auto first_non_space = line.find_first_not_of(" \t");
    if (first_non_space != std::string::npos) {
      line.erase(0, first_non_space);
    } else {
      line.clear();
    }
    indent(os, level);
    os << line << "\n";
  }
}

void dump_matrix(std::ostream& os, const char* label,
                 const double (&matrix)[9]) {
  os << "    " << label << ":\n";
  os << "      [" << matrix[0] << ", " << matrix[1] << ", " << matrix[2]
     << "]\n";
  os << "      [" << matrix[3] << ", " << matrix[4] << ", " << matrix[5]
     << "]\n";
  os << "      [" << matrix[6] << ", " << matrix[7] << ", " << matrix[8]
     << "]\n";
}
}  // namespace

std::string format_node_metadata(const NodeInspectionView& node) {
  std::ostringstream os;
  if (!node.has_cached_output || !node.debug || !node.space) {
    os << "  (No cached output available)\n";
    return os.str();
  }

  os << "  [Source] " << node.source_label.value_or("unknown") << "\n";

  const auto& debug = *node.debug;
  const auto& space = *node.space;

  os << "  [Debug]\n";
  os << "    Worker ID: " << debug.computed_by_worker_id << "\n";
  os << "    Device:    " << debug.compute_device << "\n";
  os << "    Timestamp: " << debug.timestamp_us << " us\n";
  os << "    Exec(ms):  " << debug.execution_time_ms << "\n";
  os << "    Range:     [" << debug.min_val << ", " << debug.max_val << "]";
  if (debug.has_nan) {
    os << " (NaN detected)";
  }
  os << "\n";

  os << "  [Spatial]\n";
  os << "    Scale:     " << space.global_scale_x << " x "
     << space.global_scale_y << "\n";
  os << "    ROI:       (" << space.absolute_roi.x << ", "
     << space.absolute_roi.y << ", " << space.absolute_roi.width << "x"
     << space.absolute_roi.height << ")\n";

  dump_matrix(os, "Transform", space.transform_matrix);
  dump_matrix(os, "Inverse (Global)", space.inverse_matrix);

  return os.str();
}

std::string format_node_inspection(const NodeInspectionView& node) {
  std::ostringstream os;
  os << "Node " << node.id.value << " (" << node.name << " | " << node.type
     << ":" << node.subtype << ")\n";
  os << format_node_metadata(node);
  return os.str();
}

std::string format_graph_inspection(const GraphInspectionView& graph) {
  std::ostringstream os;
  for (size_t idx = 0; idx < graph.nodes.size(); ++idx) {
    const auto& node = graph.nodes[idx];
    os << "=== Node " << node.id.value << " (" << node.name << " | "
       << node.type << ":" << node.subtype << ") ===\n";
    os << format_node_metadata(node);
    if (idx + 1 < graph.nodes.size()) {
      os << "\n";
    }
  }
  return os.str();
}

std::string format_dependency_tree(const HostDependencyTreeSnapshot& tree,
                                   bool show_parameters, bool show_metadata) {
  std::ostringstream os;
  if (tree.scope == HostDependencyTreeScope::StartNode) {
    os << "Dependency Tree (starting from Node "
       << (tree.start_node ? tree.start_node->value : -1) << "):\n";
  } else {
    os << "Dependency Tree (reversed from ending nodes):\n";
  }

  if (tree.scope == HostDependencyTreeScope::StartNode &&
      !tree.start_node_found) {
    os << "(Node " << (tree.start_node ? tree.start_node->value : -1)
       << " not found in graph)\n";
    return os.str();
  }

  if (tree.graph_empty) {
    os << "(Graph is empty)\n";
    return os.str();
  }

  if (tree.scope == HostDependencyTreeScope::EndingNodes &&
      tree.no_ending_nodes) {
    os << "(Graph has cycles or is fully connected)\n";
    return os.str();
  }

  for (const auto& entry : tree.entries) {
    if (entry.incoming_edge) {
      os << "\n";
      indent(os, std::max(0, entry.depth - 1));
      const auto& edge = *entry.incoming_edge;
      if (edge.kind == HostGraphEdgeKind::ImageInput) {
        os << "(image from " << edge.from_node.value << ":"
           << edge.from_output_name << ")\n";
      } else {
        os << "(param '" << edge.to_input_name << "' from "
           << edge.from_node.value << ":" << edge.from_output_name << ")\n";
      }
    }

    os << "\n";
    indent(os, entry.depth);
    if (entry.cycle) {
      os << "- ... (Cycle detected on Node " << entry.node.id.value
         << ") ...\n";
      continue;
    }

    os << "- Node " << entry.node.id.value << " (" << entry.node.name << " | "
       << entry.node.type << ":" << entry.node.subtype << ")\n";

    if (show_metadata) {
      dump_trimmed_metadata(os, entry.node, entry.depth + 1);
    }

    if (show_parameters) {
      dump_parameters(os, entry.node.parameters, entry.depth);
    }
  }

  return os.str();
}

}  // namespace ps::cli
