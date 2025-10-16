// FILE: src/cli/command/command_read.cpp
#include <iostream>
#include <sstream>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

bool handle_read(std::istringstream& iss, ps::InteractionService& svc,
                 std::string& current_graph, bool& modified,
                 CliConfig& /*config*/) {
  if (current_graph.empty()) {
    std::cout << "No current graph. Use load/switch.\n";
    return true;
  }
  std::string path;
  iss >> path;
  if (path.empty())
    std::cout << "Usage: read <filepath>\n";
  else {
    if (svc.cmd_reload_yaml(current_graph, path)) {
      modified = false;
      std::cout << "Loaded graph from " << path << "\n";
    } else {
      std::cout << "Failed to load." << std::endl;
    }
  }
  return true;
}

void print_help_read(const CliConfig& /*config*/) {
  print_help_from_file("help_read.txt");
}
