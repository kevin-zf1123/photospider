// FILE: src/cli/command/command_inspect.cpp
#include <iostream>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "cli/dependency_tree_formatter.hpp"

bool handle_inspect(std::istringstream& iss, ps::InteractionService& svc,
                    std::string& current_graph, bool& /*modified*/,
                    CliConfig& /*config*/) {
  if (current_graph.empty()) {
    std::cout << "No current graph. Use load/switch.\n";
    return true;
  }
  std::string target;
  if (!(iss >> target)) {
    std::cout << "Usage: inspect <node_id>|all\n";
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
