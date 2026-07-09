// FILE: src/cli/command/command_print.cpp
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "cli/dependency_tree_formatter.hpp"

bool handle_print(std::istringstream& iss, ps::Host& svc,
                  std::string& current_graph, bool& /*modified*/,
                  CliConfig& config) {
  if (current_graph.empty()) {
    std::cout << "No current graph. Use load/switch.\n";
    return true;
  }
  std::string target_str = "all";
  std::string mode_str = config.default_print_mode;
  bool target_is_set = false;
  bool show_metadata = false;

  std::string arg;
  while (iss >> arg) {
    if (arg == "f" || arg == "full" || arg == "s" || arg == "simplified") {
      mode_str = arg;
    } else if (arg == "i" || arg == "inspect") {
      show_metadata = true;
    } else {
      if (target_is_set) {
        std::cout << "Warning: Multiple targets specified for print; using "
                     "last one ('"
                  << arg << "').\n";
      }
      target_str = arg;
      target_is_set = true;
    }
  }

  bool show_params = (mode_str == "f" || mode_str == "full");
  std::optional<int> target_node;
  if (target_str != "all") {
    try {
      target_node = std::stoi(target_str);
    } catch (const std::exception&) {
      std::cout << "Error: Invalid target '" << target_str
                << "'. Must be an integer ID or 'all'." << std::endl;
      return true;
    }
  }

  std::optional<ps::NodeId> target;
  if (target_node) {
    target = ps::NodeId{*target_node};
  }
  auto tree = svc.dependency_tree(ps::GraphSessionId{current_graph}, target,
                                  show_metadata);
  if (tree.status.ok)
    std::cout << ps::cli::format_dependency_tree(tree.value, show_params,
                                                 show_metadata);
  else
    std::cout << "(failed to dump tree)\n";
  return true;
}

void print_help_print(const CliConfig& /*config*/) {
  print_help_from_file("help_print.txt");
}
