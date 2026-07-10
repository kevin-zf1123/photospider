// FILE: src/cli/command/command_output.cpp
#include <iostream>
#include <sstream>
#include <string>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

/** @copydoc handle_output */
bool handle_output(std::istringstream& iss, ps::Host& svc,
                   std::string& current_graph, bool& modified,
                   CliConfig& /*config*/) {
  if (current_graph.empty()) {
    std::cout << "No current graph. Use load/switch.\n";
    return true;
  }
  std::string path;
  iss >> path;
  if (path.empty()) {
    std::cout << "Usage: output <filepath>\n";
  } else {
    if (svc.save_graph(ps::GraphSessionId{current_graph}, path).status.ok) {
      modified = false;
      std::cout << "Saved to " << path << "\n";
    } else {
      std::cout << "Failed to save." << std::endl;
    }
  }
  return true;
}

/** @copydoc print_help_output */
void print_help_output(const CliConfig& /*config*/) {
  print_help_from_file("help_output.txt");
}
