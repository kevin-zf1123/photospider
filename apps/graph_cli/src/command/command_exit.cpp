// FILE: apps/graph_cli/src/command/command_exit.cpp
#include <iostream>
#include <sstream>
#include <string>

#include "graph_cli/ask_yesno.hpp"
#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"

/** @copydoc handle_exit */
bool handle_exit(std::istringstream& /*iss*/, ps::Host& svc,
                 std::string& current_graph, bool& /*modified*/,
                 CliConfig& config) {
  if (!current_graph.empty() &&
      ask_yesno("Synchronize disk cache with memory state before exiting?",
                config.exit_prompt_sync)) {
    svc.synchronize_disk_cache(ps::GraphSessionId{current_graph},
                               config.cache_precision);
  }
  return false;  // signal REPL exit
}

/** @copydoc print_help_exit */
void print_help_exit(const CliConfig& /*config*/) {
  print_help_from_file("help_exit.txt");
}
