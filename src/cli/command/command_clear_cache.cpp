// FILE: src/cli/command/command_clear_cache.cpp
#include <iostream>
#include <sstream>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

bool handle_clear_cache(std::istringstream& iss, ps::InteractionService& svc,
                        std::string& current_graph, bool& /*modified*/,
                        CliConfig& config) {
  if (current_graph.empty()) {
    std::cout << "No current graph. Use load/switch.\n";
    return true;
  }
  std::string arg;
  iss >> arg;
  if (arg.empty()) {
    arg = config.default_cache_clear_arg;
  }
  if (arg == "both" || arg == "md" || arg == "dm")
    svc.cmd_clear_cache(current_graph);
  else if (arg == "drive" || arg == "d")
    svc.cmd_clear_drive_cache(current_graph);
  else if (arg == "memory" || arg == "m")
    svc.cmd_clear_memory_cache(current_graph);
  else {
    std::cout << "Error: Invalid argument for clear-cache. Use: m, d, or md."
              << std::endl;
  }
  return true;
}

void print_help_clear_cache(const CliConfig& /*config*/) {
  print_help_from_file("help_clear-cache.txt");
}
