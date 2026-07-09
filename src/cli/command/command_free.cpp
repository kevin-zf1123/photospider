// FILE: src/cli/command/command_free.cpp
#include <iostream>
#include <sstream>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

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

void print_help_free(const CliConfig& /*config*/) {
  print_help_from_file("help_free.txt");
}
