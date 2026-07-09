// FILE: src/cli/command/command_graphs.cpp
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

bool handle_graphs(std::istringstream& /*iss*/, ps::Host& svc,
                   std::string& current_graph, bool& /*modified*/,
                   CliConfig& /*config*/) {
  auto result = svc.list_graphs();
  if (!result.status.ok || result.value.empty()) {
    std::cout << "(no graphs loaded)\n";
    return true;
  }
  std::cout << "Loaded graphs:" << std::endl;
  for (const auto& session : result.value) {
    const auto& n = session.value;
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
