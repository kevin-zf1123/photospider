// FILE: apps/graph_cli/src/command/command_help.cpp
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"
#include "graph_cli/print_repl_help.hpp"

/**
 * @brief Canonicalizes supported command aliases for help dispatch.
 * @param cmd Command or alias read from the help argument.
 * @return Canonical command name, or cmd unchanged when no alias exists.
 * @throws std::bad_alloc if static alias-map initialization or result copying
 * cannot allocate.
 * @note The immutable alias table has process lifetime and no Host state.
 */
static std::string canonicalize(const std::string& cmd) {
  static const std::unordered_map<std::string, std::string> alias = {
      {"cls", "clear"},
      {"cc", "clear-cache"},
      {"q", "exit"},
      {"quit", "exit"}};
  auto it = alias.find(cmd);
  return (it == alias.end()) ? cmd : it->second;
}

/**
 * @brief Dispatches one canonical command name to its help renderer.
 * @param name Command name or supported alias.
 * @param config Borrowed CLI configuration forwarded to the renderer.
 * @return True when a matching help renderer ran; false otherwise.
 * @throws std::bad_alloc if canonicalization or help rendering cannot allocate.
 * @note Dispatch is synchronous and does not access Host or graph/cache state.
 */
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
  } else if (cmd == "inspect") {
    print_help_inspect(config);
    return true;
  } else if (cmd == "exit") {
    print_help_exit(config);
    return true;
  } else if (cmd == "policy") {
    print_help_policy(config);
    return true;
  } else if (cmd == "execution") {
    print_help_execution(config);
    return true;
  }
  return false;
}

/** @copydoc handle_help */
bool handle_help(std::istringstream& iss, ps::Host& /*svc*/,
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

/** @copydoc print_help_help */
void print_help_help(const CliConfig& /*config*/) {
  print_help_from_file("help_help.txt");
}
