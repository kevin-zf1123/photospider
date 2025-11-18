// FILE: src/cli/command/command_inspect.cpp
#include <iostream>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

bool handle_inspect(std::istringstream& iss, ps::InteractionService& svc,
                    std::string& current_graph, bool& /*modified*/,
                    CliConfig& /*config*/) {
  if (current_graph.empty()) {
    std::cout << "No current graph. Use load/switch.\n";
    return true;
  }
  std::string node_str;
  if (!(iss >> node_str)) {
    std::cout << "Usage: inspect <node_id>\n";
    return true;
  }
  int node_id = -1;
  try {
    node_id = std::stoi(node_str);
  } catch (const std::exception&) {
    std::cout << "Invalid node id '" << node_str << "'.\n";
    return true;
  }
  auto report = svc.cmd_inspect_node(current_graph, node_id);
  if (!report) {
    std::cout << "Unable to inspect node " << node_id << ".\n";
    return true;
  }
  std::cout << *report;
  if (!report->empty() && report->back() != '\n')
    std::cout << '\n';
  return true;
}

void print_help_inspect(const CliConfig& /*config*/) {
  print_help_from_file("help_inspect.txt");
}
