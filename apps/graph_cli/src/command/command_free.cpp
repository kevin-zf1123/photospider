// FILE: apps/graph_cli/src/command/command_free.cpp
#include <iostream>
#include <sstream>
#include <string>

#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"

/** @copydoc handle_free */
bool handle_free(std::istringstream& /*iss*/, ps::Host& svc,
                 std::string& current_graph, bool& /*modified*/,
                 CliConfig& /*config*/) {
  if (current_graph.empty()) {
    std::cout << "No current graph. Use load/switch.\n";
    return true;
  }
  svc.free_transient_memory(ps::GraphSessionId{current_graph});
  std::cout << "Freed intermediate memory." << std::endl;
  return true;
}

/** @copydoc print_help_free */
void print_help_free(const CliConfig& /*config*/) {
  print_help_from_file("help_free.txt");
}
