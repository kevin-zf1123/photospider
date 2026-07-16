#include "graph_cli/dependency_tree_formatter.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace ps::cli {

namespace {
/**
 * @brief Writes two-space indentation for one tree-output line.
 *
 * @param os Borrowed destination stream.
 * @param level Non-negative indentation depth; negative values emit nothing.
 * @return Nothing.
 * @throws std::ios_base::failure if the caller enabled stream exceptions and
 *         writing fails.
 * @note The helper neither owns nor retains `os` and performs no Host access.
 */
void indent(std::ostream& os, int level) {
  for (int i = 0; i < level; ++i) {
    os << "  ";
  }
}

/**
 * @brief Writes copied static node parameters below a dependency-tree row.
 *
 * @param os Borrowed destination stream.
 * @param parameters Ordered copied parameter map from a Host node snapshot.
 * @param level Parent dependency-tree indentation depth.
 * @return Nothing; an empty map emits no text.
 * @throws std::bad_alloc if stream buffering exhausts memory.
 * @throws std::ios_base::failure if the caller enabled stream exceptions and
 *         writing fails.
 * @note Map order is preserved. The helper retains neither the stream nor the
 *       copied parameter strings and performs no Host call.
 */
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

/**
 * @brief Writes node metadata with normalized indentation.
 *
 * The helper formats the copied node metadata, removes carriage returns and
 * leading horizontal whitespace line by line, and writes every line at the
 * requested dependency-tree depth.
 *
 * @param os Borrowed destination stream.
 * @param node Copied public Host node snapshot to format.
 * @param level Output indentation depth.
 * @return Nothing.
 * @throws std::bad_alloc if metadata, line, or stream-buffer construction
 *         exhausts memory.
 * @throws std::ios_base::failure if the caller enabled stream exceptions and
 *         reading or writing fails.
 * @note The helper retains neither `os` nor `node` and performs no Host call.
 */
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

/**
 * @brief Writes one row-major 3-by-3 spatial matrix.
 *
 * @param os Borrowed destination stream.
 * @param label Borrowed null-terminated label valid for the duration of the
 *        call.
 * @param matrix Borrowed nine-element row-major matrix.
 * @return Nothing.
 * @throws std::bad_alloc if stream buffering exhausts memory.
 * @throws std::ios_base::failure if the caller enabled stream exceptions and
 *         writing fails.
 * @note The helper retains no borrowed input and performs no Host call.
 */
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

/**
 * @brief Returns the CLI label for a public dirty-domain value.
 *
 * @param domain Public dirty domain from a Host snapshot.
 * @return Stable short label used in `inspect dirty` output.
 * @throws Nothing.
 * @note The returned string has static storage; unknown enum values map to
 *       `unknown` and no Host state is accessed.
 */
const char* dirty_domain_name(DirtyDomain domain) {
  switch (domain) {
    case DirtyDomain::HighPrecision:
      return "hp";
    case DirtyDomain::RealTime:
      return "rt";
  }
  return "unknown";
}

/**
 * @brief Returns the CLI label for a dirty-source lifecycle value.
 *
 * @param lifecycle Public dirty-source lifecycle value.
 * @return Stable lifecycle label used in `inspect dirty` output.
 * @throws Nothing.
 * @note The returned string has static storage; unknown enum values map to
 *       `unknown` and no Host state is accessed.
 */
const char* dirty_source_lifecycle_name(DirtySourceLifecycleState lifecycle) {
  switch (lifecycle) {
    case DirtySourceLifecycleState::Idle:
      return "idle";
    case DirtySourceLifecycleState::Updating:
      return "updating";
    case DirtySourceLifecycleState::Settled:
      return "settled";
  }
  return "unknown";
}

/**
 * @brief Returns the CLI label for a dirty edge mapping direction.
 *
 * @param direction Public dirty edge mapping direction.
 * @return Stable direction label used in `inspect dirty` output.
 * @throws Nothing.
 * @note The returned string has static storage; unknown enum values map to
 *       `unknown` and no Host state is accessed.
 */
const char* dirty_edge_direction_name(DirtyEdgeDirection direction) {
  switch (direction) {
    case DirtyEdgeDirection::ForwardAffected:
      return "forward-affected";
    case DirtyEdgeDirection::BackwardDemand:
      return "backward-demand";
  }
  return "unknown";
}

/**
 * @brief Formats one public pixel rectangle for dirty inspection output.
 *
 * @param rect Pixel rectangle to format.
 * @return Compact `x,y widthxheight` text.
 * @throws std::bad_alloc if string construction allocates and fails.
 * @note The helper consumes a copied public value, performs no Host call, and
 *       retains no reference to `rect`.
 */
std::string rect_text(const PixelRect& rect) {
  std::ostringstream out;
  out << rect.x << "," << rect.y << " " << rect.width << "x" << rect.height;
  return out.str();
}
}  // namespace

/** @copydoc format_node_metadata */
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
  dump_matrix(os, "Inverse (Local)", space.local_inverse_matrix);

  return os.str();
}

/** @copydoc format_node_inspection */
std::string format_node_inspection(const NodeInspectionView& node) {
  std::ostringstream os;
  os << "Node " << node.id.value << " (" << node.name << " | " << node.type
     << ":" << node.subtype << ")\n";
  os << format_node_metadata(node);
  return os.str();
}

/** @copydoc format_graph_inspection */
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

/** @copydoc format_dirty_snapshot */
std::string format_dirty_snapshot(
    const DirtyRegionInspectionSnapshot& snapshot) {
  std::ostringstream out;
  if (snapshot.graph_generation == 0 && snapshot.sources.empty() &&
      snapshot.dirty_tiles.empty() && snapshot.dirty_monolithic_nodes.empty() &&
      snapshot.actual_dirty_rois.empty() && snapshot.edge_mappings.empty()) {
    out << "(No dirty snapshot recorded.)\n";
    return out.str();
  }

  out << "Generation: " << snapshot.graph_generation << "\n";
  out << "Dirty sources: " << snapshot.sources.size() << "\n";
  for (const auto& source : snapshot.sources) {
    out << "  node " << source.node.value << " "
        << dirty_domain_name(source.domain) << " "
        << dirty_source_lifecycle_name(source.lifecycle)
        << " generation=" << source.generation;
    for (const auto& roi : source.source_rois) {
      out << " [" << rect_text(roi) << "]";
    }
    out << "\n";
  }

  out << "Dirty tiles: " << snapshot.dirty_tiles.size() << "\n";
  for (const auto& tile : snapshot.dirty_tiles) {
    out << "  node " << tile.node.value << " " << dirty_domain_name(tile.domain)
        << " tile(" << tile.tile_x << "," << tile.tile_y
        << ") size=" << tile.tile_size << " roi=" << rect_text(tile.pixel_roi)
        << "\n";
  }

  out << "Monolithic dirty regions: " << snapshot.dirty_monolithic_nodes.size()
      << "\n";
  for (const auto& region : snapshot.dirty_monolithic_nodes) {
    out << "  node " << region.node.value << " "
        << dirty_domain_name(region.domain)
        << " whole=" << (region.whole_output ? "true" : "false")
        << " roi=" << rect_text(region.pixel_roi) << "\n";
  }

  out << "Per-node dirty ROIs: " << snapshot.actual_dirty_rois.size() << "\n";
  std::vector<int> roi_node_ids;
  roi_node_ids.reserve(snapshot.actual_dirty_rois.size());
  for (const auto& [node_id, _] : snapshot.actual_dirty_rois) {
    roi_node_ids.push_back(node_id);
  }
  std::sort(roi_node_ids.begin(), roi_node_ids.end());
  for (int node_id : roi_node_ids) {
    out << "  node " << node_id << ":";
    for (const auto& roi : snapshot.actual_dirty_rois.at(node_id)) {
      out << " [" << rect_text(roi) << "]";
    }
    out << "\n";
  }

  out << "Edge mappings: " << snapshot.edge_mappings.size() << "\n";
  for (const auto& mapping : snapshot.edge_mappings) {
    out << "  node " << mapping.from_node.value << " -> "
        << mapping.to_node.value << " " << dirty_domain_name(mapping.domain)
        << " " << dirty_edge_direction_name(mapping.direction) << " from=["
        << rect_text(mapping.from_roi) << "]";
    out << " to=[" << rect_text(mapping.to_roi) << "]\n";
  }

  return out.str();
}

/** @copydoc format_dependency_tree */
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
