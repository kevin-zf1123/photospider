// FILE: apps/graph_cli/src/command/command_inspect.cpp
#include <iostream>
#include <new>
#include <sstream>
#include <string>

#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"
#include "graph_cli/dependency_tree_formatter.hpp"

/** @copydoc handle_inspect */
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
      std::cout << "Unable to inspect dirty regions for graph '"
                << current_graph << "'.\n";
      if (!snapshot.status.message.empty()) {
        std::cout << "Reason: " << snapshot.status.message << "\n";
      }
      return true;
    }
    std::cout << ps::cli::format_dirty_snapshot(snapshot.value);
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
  } catch (const std::bad_alloc&) {
    throw;
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

/** @copydoc print_help_inspect */
void print_help_inspect(const CliConfig& /*config*/) {
  print_help_from_file("help_inspect.txt");
}
