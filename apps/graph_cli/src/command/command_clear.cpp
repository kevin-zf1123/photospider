// FILE: apps/graph_cli/src/command/command_clear.cpp
#include <iostream>
#include <sstream>
#include <string>

#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"

/** @copydoc handle_clear */
bool handle_clear(std::istringstream& /*iss*/, ps::Host& /*svc*/,
                  std::string& /*current_graph*/, bool& /*modified*/,
                  CliConfig& /*config*/) {
  std::cout << "\033[2J\033[1;1H";
  return true;
}

/** @copydoc print_help_clear */
void print_help_clear(const CliConfig& /*config*/) {
  print_help_from_file("help_clear.txt");
}
