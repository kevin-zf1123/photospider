// FILE: apps/graph_cli/src/command/command_graphs.cpp
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"

/** @copydoc handle_graphs */
bool handle_graphs(std::istringstream& /*iss*/, ps::Host& svc,
                   std::string& current_graph, bool& /*modified*/,
                   CliConfig& /*config*/) {
  auto result = svc.list_graphs();
  if (!result.status.ok || result.value.empty()) {
    std::cout << "(no graphs loaded)\n";
    return true;
  }
  std::cout << "Loaded graphs:" << std::endl;
  for (const auto& session : result.value) {
    const auto& n = session.value;
    std::cout << "  - " << n;
    if (n == current_graph)
      std::cout << "  [current]";
    std::cout << std::endl;
  }
  return true;
}

/** @copydoc print_help_graphs */
void print_help_graphs(const CliConfig& /*config*/) {
  print_help_from_file("help_graphs.txt");
}
