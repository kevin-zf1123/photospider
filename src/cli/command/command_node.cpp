// FILE: src/cli/command/command_node.cpp
#include <iostream>
#include <optional>
#include <sstream>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "cli/node_editor_full.hpp"

bool handle_node(std::istringstream& iss, ps::InteractionService& svc,
                 std::string& current_graph, bool& /*modified*/,
                 CliConfig& /*config*/) {
  if (current_graph.empty()) {
    std::cout << "No current graph. Use load/switch.\n";
    return true;
  }
  std::optional<int> maybe_id;
  std::string word;
  if (iss >> word) {
    try {
      maybe_id = std::stoi(word);
    } catch (...) {
      maybe_id.reset();
    }
  }
  run_node_editor_full(svc, current_graph, maybe_id);
  std::cout << "\033[2J\033[1;1H";
  return true;
}

void print_help_node(const CliConfig& /*config*/) {
  print_help_from_file("help_node.txt");
}
