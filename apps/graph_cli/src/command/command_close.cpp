// FILE: apps/graph_cli/src/command/command_close.cpp
#include <iostream>
#include <sstream>
#include <string>

#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"

/** @copydoc handle_close */
bool handle_close(std::istringstream& iss, ps::Host& svc,
                  std::string& current_graph, bool& /*modified*/,
                  CliConfig& /*config*/) {
  std::string name;
  iss >> name;
  if (name.empty()) {
    std::cout << "Usage: close <name>\n";
    return true;
  }
  if (!svc.close_graph(ps::GraphSessionId{name}).status.ok) {
    std::cout << "Error: failed to close '" << name << "'.\n";
    return true;
  }
  if (current_graph == name)
    current_graph.clear();
  std::cout << "Closed graph '" << name << "'.\n";
  return true;
}

/** @copydoc print_help_close */
void print_help_close(const CliConfig& /*config*/) {
  print_help_from_file("help_close.txt");
}
