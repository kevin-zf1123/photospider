// FILE: src/cli/command/command_config.cpp
#include <sstream>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "cli/config_editor.hpp"

bool handle_config(std::istringstream& /*iss*/, ps::InteractionService& /*svc*/,
                   std::string& /*current_graph*/, bool& /*modified*/,
                   CliConfig& config) {
  run_config_editor(config);
  return true;
}

void print_help_config(const CliConfig& /*config*/) {
  print_help_from_file("help_config.txt");
}
