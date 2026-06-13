// FILE: src/cli/command/command_inspect.cpp
#include <algorithm>
#include <iostream>
#include <sstream>
#include <vector>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "cli/dependency_tree_formatter.hpp"

namespace {

const char* dirty_domain_name(ps::compute::DirtyDomain domain) {
  switch (domain) {
    case ps::compute::DirtyDomain::HighPrecision:
      return "hp";
    case ps::compute::DirtyDomain::RealTime:
      return "rt";
  }
  return "unknown";
}

const char* dirty_tile_level_name(ps::compute::DirtyTileLevel level) {
  switch (level) {
    case ps::compute::DirtyTileLevel::Micro:
      return "micro";
    case ps::compute::DirtyTileLevel::Macro:
      return "macro";
  }
  return "unknown";
}

const char* dirty_edge_direction_name(
    ps::compute::DirtyEdgeDirection direction) {
  switch (direction) {
    case ps::compute::DirtyEdgeDirection::ForwardAffected:
      return "forward";
    case ps::compute::DirtyEdgeDirection::BackwardDemand:
      return "backward";
  }
  return "unknown";
}

std::string rect_text(const cv::Rect& rect) {
  std::ostringstream out;
  out << rect.x << "," << rect.y << " " << rect.width << "x" << rect.height;
  return out.str();
}

std::string format_dirty_snapshot(
    const std::optional<std::string>& debug,
    const std::optional<ps::compute::DirtyRegionSnapshot>& snapshot) {
  std::ostringstream out;
  if (debug) {
    out << "Dirty snapshot: " << *debug << "\n";
  }
  if (!snapshot) {
    out << "(No dirty snapshot recorded.)\n";
    return out.str();
  }

  const auto& snap = *snapshot;
  out << "Generation: " << snap.graph_generation << "\n";
  out << "Dirty sources: " << snap.dirty_source_nodes.size() << "\n";
  if (!snap.dirty_source_nodes.empty()) {
    std::vector<int> source_ids = snap.dirty_source_nodes;
    std::sort(source_ids.begin(), source_ids.end());
    out << "  nodes:";
    for (int node_id : source_ids) {
      out << " " << node_id;
    }
    out << "\n";
  }
  out << "Updating sources: " << snap.dirty_updating_count << "\n";
  out << "Dirty tiles: " << snap.dirty_tiles.size() << "\n";
  for (const auto& tile : snap.dirty_tiles) {
    out << "  node " << tile.node_id << " " << dirty_domain_name(tile.domain)
        << " " << dirty_tile_level_name(tile.level) << " tile("
        << tile.tile_x << "," << tile.tile_y << ") size=" << tile.tile_size
        << " roi=" << rect_text(tile.pixel_roi) << "\n";
  }
  out << "Monolithic dirty regions: " << snap.dirty_monolithic_nodes.size()
      << "\n";
  for (const auto& region : snap.dirty_monolithic_nodes) {
    out << "  node " << region.node_id << " "
        << dirty_domain_name(region.domain) << " roi="
        << rect_text(region.pixel_roi)
        << " whole_output=" << (region.whole_output ? "true" : "false")
        << "\n";
  }
  out << "Per-node dirty ROIs: " << snap.per_node_dirty_rois.size() << "\n";
  std::vector<int> roi_node_ids;
  roi_node_ids.reserve(snap.per_node_dirty_rois.size());
  for (const auto& [node_id, _] : snap.per_node_dirty_rois) {
    roi_node_ids.push_back(node_id);
  }
  std::sort(roi_node_ids.begin(), roi_node_ids.end());
  for (int node_id : roi_node_ids) {
    out << "  node " << node_id << ":";
    for (const auto& roi : snap.per_node_dirty_rois.at(node_id)) {
      out << " [" << rect_text(roi) << "]";
    }
    out << "\n";
  }
  out << "Edge mappings: " << snap.edge_mappings.size() << "\n";
  for (const auto& edge : snap.edge_mappings) {
    out << "  " << edge.from_node_id << " -> " << edge.to_node_id << " "
        << dirty_domain_name(edge.domain) << " "
        << dirty_edge_direction_name(edge.direction) << " from="
        << rect_text(edge.from_roi) << " to=" << rect_text(edge.to_roi)
        << "\n";
  }
  return out.str();
}

}  // namespace

bool handle_inspect(std::istringstream& iss, ps::InteractionService& svc,
                    std::string& current_graph, bool& /*modified*/,
                    CliConfig& /*config*/) {
  if (current_graph.empty()) {
    std::cout << "No current graph. Use load/switch.\n";
    return true;
  }
  std::string target;
  if (!(iss >> target)) {
    std::cout << "Usage: inspect <node_id>|all|dirty\n";
    return true;
  }

  if (target == "dirty") {
    auto debug = svc.cmd_dirty_region_snapshot_debug(current_graph);
    auto snapshot = svc.cmd_dirty_region_snapshot(current_graph);
    std::cout << format_dirty_snapshot(debug, snapshot);
    return true;
  }

  if (target == "all") {
    auto report = svc.cmd_inspect_graph(current_graph);
    if (!report) {
      std::cout << "Unable to inspect graph.\n";
      return true;
    }
    if (!report->nodes.empty())
      std::cout << ps::cli::format_graph_inspection(*report) << "\n";
    else
      std::cout << "(No inspectable nodes)\n";
    return true;
  }

  int node_id = -1;
  try {
    node_id = std::stoi(target);
  } catch (const std::exception&) {
    std::cout << "Invalid node id '" << target << "'.\n";
    return true;
  }
  auto report = svc.cmd_inspect_node(current_graph, node_id);
  if (!report) {
    std::cout << "Unable to inspect node " << node_id << ".\n";
    return true;
  }
  std::string text = ps::cli::format_node_inspection(*report);
  std::cout << text;
  if (!text.empty() && text.back() != '\n')
    std::cout << '\n';
  return true;
}

void print_help_inspect(const CliConfig& /*config*/) {
  print_help_from_file("help_inspect.txt");
}
