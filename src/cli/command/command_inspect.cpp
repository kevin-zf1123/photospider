// FILE: src/cli/command/command_inspect.cpp
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "cli/dependency_tree_formatter.hpp"

namespace {

const char* dirty_domain_name(ps::DirtyDomain domain) {
  switch (domain) {
    case ps::DirtyDomain::HighPrecision:
      return "hp";
    case ps::DirtyDomain::RealTime:
      return "rt";
  }
  return "unknown";
}

const char* dirty_source_lifecycle_name(
    ps::DirtySourceLifecycleState lifecycle) {
  switch (lifecycle) {
    case ps::DirtySourceLifecycleState::Idle:
      return "idle";
    case ps::DirtySourceLifecycleState::Updating:
      return "updating";
    case ps::DirtySourceLifecycleState::Settled:
      return "settled";
  }
  return "unknown";
}

std::string rect_text(const ps::PixelRect& rect) {
  std::ostringstream out;
  out << rect.x << "," << rect.y << " " << rect.width << "x" << rect.height;
  return out.str();
}

std::string format_dirty_snapshot(
    const ps::DirtyRegionInspectionSnapshot& snapshot) {
  std::ostringstream out;
  if (snapshot.graph_generation == 0 && snapshot.sources.empty() &&
      snapshot.dirty_tiles.empty() && snapshot.actual_dirty_rois.empty()) {
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
  return out.str();
}

}  // namespace

bool handle_inspect(std::istringstream& iss, ps::Host& svc,
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
    auto snapshot =
        svc.dirty_region_snapshot(ps::GraphSessionId{current_graph});
    if (!snapshot.status.ok) {
      std::cout << "(No dirty snapshot recorded.)\n";
      return true;
    }
    std::cout << format_dirty_snapshot(snapshot.value);
    return true;
  }

  if (target == "all") {
    auto report = svc.inspect_graph(ps::GraphSessionId{current_graph});
    if (!report.status.ok) {
      std::cout << "Unable to inspect graph.\n";
      return true;
    }
    if (!report.value.nodes.empty())
      std::cout << ps::cli::format_graph_inspection(report.value) << "\n";
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
  auto report =
      svc.inspect_node(ps::GraphSessionId{current_graph}, ps::NodeId{node_id});
  if (!report.status.ok) {
    std::cout << "Unable to inspect node " << node_id << ".\n";
    return true;
  }
  std::string text = ps::cli::format_node_inspection(report.value);
  std::cout << text;
  if (!text.empty() && text.back() != '\n')
    std::cout << '\n';
  return true;
}

void print_help_inspect(const CliConfig& /*config*/) {
  print_help_from_file("help_inspect.txt");
}
