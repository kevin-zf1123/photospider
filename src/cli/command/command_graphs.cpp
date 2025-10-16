// FILE: src/cli/command/command_graphs.cpp
#include <iostream>
#include <sstream>
#include <vector>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

bool handle_graphs(std::istringstream& /*iss*/, ps::InteractionService& svc,
                   std::string& current_graph, bool& /*modified*/,
                   CliConfig& /*config*/) {
  auto names = svc.cmd_list_graphs();
  if (names.empty()) {
    std::cout << "(no graphs loaded)\n";
    return true;
  }
  std::cout << "Loaded graphs:" << std::endl;
  for (auto& n : names) {
    std::cout << "  - " << n;
    if (n == current_graph)
      std::cout << "  [current]";
    std::cout << std::endl;
  }
  return true;
}

void print_help_graphs(const CliConfig& /*config*/) {
  print_help_from_file("help_graphs.txt");
}
