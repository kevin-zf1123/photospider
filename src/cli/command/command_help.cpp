// FILE: src/cli/command/command_help.cpp
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "cli/print_repl_help.hpp"

// Helper to canonicalize command names and aliases
static std::string canonicalize(const std::string& cmd) {
  static const std::unordered_map<std::string, std::string> alias = {
      {"cls", "clear"},
      {"cc", "clear-cache"},
      {"q", "exit"},
      {"quit", "exit"}};
  auto it = alias.find(cmd);
  return (it == alias.end()) ? cmd : it->second;
}

// Dispatcher for printing specific command help
static bool dispatch_print(const std::string& name, const CliConfig& config) {
  const std::string cmd = canonicalize(name);
  if (cmd == "help") {
    print_help_help(config);
    return true;
  } else if (cmd == "clear") {
    print_help_clear(config);
    return true;
  } else if (cmd == "graphs") {
    print_help_graphs(config);
    return true;
  } else if (cmd == "load") {
    print_help_load(config);
    return true;
  } else if (cmd == "switch") {
    print_help_switch(config);
    return true;
  } else if (cmd == "close") {
    print_help_close(config);
    return true;
  } else if (cmd == "print") {
    print_help_print(config);
    return true;
  } else if (cmd == "node") {
    print_help_node(config);
    return true;
  } else if (cmd == "ops") {
    print_help_ops(config);
    return true;
  } else if (cmd == "traversal") {
    print_help_traversal(config);
    return true;
  } else if (cmd == "config") {
    print_help_config(config);
    return true;
  } else if (cmd == "read") {
    print_help_read(config);
    return true;
  } else if (cmd == "source") {
    print_help_source(config);
    return true;
  } else if (cmd == "output") {
    print_help_output(config);
    return true;
  } else if (cmd == "clear-graph") {
    print_help_clear_graph(config);
    return true;
  } else if (cmd == "clear-cache") {
    print_help_clear_cache(config);
    return true;
  } else if (cmd == "free") {
    print_help_free(config);
    return true;
  } else if (cmd == "compute") {
    print_help_compute(config);
    return true;
  } else if (cmd == "save") {
    print_help_save(config);
    return true;
  } else if (cmd == "exit") {
    print_help_exit(config);
    return true;
  }
  return false;
}

bool handle_help(std::istringstream& iss, ps::InteractionService& /*svc*/,
                 std::string& /*current_graph*/, bool& /*modified*/,
                 CliConfig& config) {
  std::string sub;
  if (!(iss >> sub)) {
    // No arg: print general REPL help
    print_repl_help(config);
    return true;
  }
  if (!dispatch_print(sub, config)) {
    std::cout << "Unknown command for help: '" << sub << "'\n";
  }
  return true;
}

void print_help_help(const CliConfig& /*config*/) {
  print_help_from_file("help_help.txt");
}
