// FILE: src/cli/command/command_read.cpp
#include <iostream>
#include <sstream>
#include <string>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

/** @copydoc handle_read */
bool handle_read(std::istringstream& iss, ps::Host& svc,
                 std::string& current_graph, bool& modified,
                 CliConfig& /*config*/) {
  if (current_graph.empty()) {
    std::cout << "No current graph. Use load/switch.\n";
    return true;
  }
  std::string path;
  iss >> path;
  if (path.empty()) {
    std::cout << "Usage: read <filepath>\n";
  } else {
    if (svc.reload_graph(ps::GraphSessionId{current_graph}, path).status.ok) {
      modified = false;
      std::cout << "Loaded graph from " << path << "\n";
    } else {
      std::cout << "Failed to load." << std::endl;
    }
  }
  return true;
}

/** @copydoc print_help_read */
void print_help_read(const CliConfig& /*config*/) {
  print_help_from_file("help_read.txt");
}
