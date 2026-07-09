// FILE: src/cli/command/command_clear_cache.cpp
#include <iostream>
#include <sstream>
#include <string>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

bool handle_clear_cache(std::istringstream& iss, ps::Host& svc,
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
  if (arg == "both" || arg == "md" || arg == "dm") {
    svc.clear_cache(ps::GraphSessionId{current_graph});
  } else if (arg == "drive" || arg == "d") {
    svc.clear_drive_cache(ps::GraphSessionId{current_graph});
  } else if (arg == "memory" || arg == "m") {
    svc.clear_memory_cache(ps::GraphSessionId{current_graph});
  } else {
    std::cout << "Error: Invalid argument for clear-cache. Use: m, d, or md."
              << std::endl;
  }
  return true;
}

void print_help_clear_cache(const CliConfig& /*config*/) {
  print_help_from_file("help_clear-cache.txt");
}
