// FILE: apps/graph_cli/src/command/command_config.cpp
#include <sstream>
#include <string>

#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"
#include "graph_cli/config_editor.hpp"

/** @copydoc handle_config */
bool handle_config(std::istringstream& /*iss*/, ps::Host& svc,
                   std::string& /*current_graph*/, bool& /*modified*/,
                   CliConfig& config) {
  if (run_config_editor(config)) {
    apply_cli_policy_execution_defaults(svc, config);
  }
  return true;
}

/** @copydoc print_help_config */
void print_help_config(const CliConfig& /*config*/) {
  print_help_from_file("help_config.txt");
}
