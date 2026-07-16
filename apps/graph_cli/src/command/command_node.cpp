// FILE: apps/graph_cli/src/command/command_node.cpp
#include <iostream>
#include <new>
#include <optional>
#include <sstream>
#include <string>

#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"
#include "graph_cli/node_editor_full.hpp"

/** @copydoc handle_node */
bool handle_node(std::istringstream& iss, ps::Host& svc,
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
    } catch (const std::bad_alloc&) {
      throw;
    } catch (...) {
      maybe_id.reset();
    }
  }
  run_node_editor_full(svc, current_graph, maybe_id);
  std::cout << "\033[2J\033[1;1H";
  return true;
}

/** @copydoc print_help_node */
void print_help_node(const CliConfig& /*config*/) {
  print_help_from_file("help_node.txt");
}
