// FILE: src/cli/process_command.cpp
#include "cli/process_command.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>

#include "cli/command/commands.hpp"
#include "cli/print_repl_help.hpp"
#include "cli_config.hpp"

namespace fs = std::filesystem;

bool process_command(const std::string& line, ps::InteractionService& svc,
                     std::string& current_graph, bool& modified,
                     CliConfig& config) {
  std::istringstream iss(line);
  std::string cmd;
  iss >> cmd;
  if (cmd.empty())
    return true;
  try {
    if (cmd == "help") {
      return handle_help(iss, svc, current_graph, modified, config);
    } else if (cmd == "clear" || cmd == "cls") {
      return handle_clear(iss, svc, current_graph, modified, config);
    } else if (cmd == "graphs") {
      return handle_graphs(iss, svc, current_graph, modified, config);
    } else if (cmd == "load") {
      return handle_load(iss, svc, current_graph, modified, config);
    } else if (cmd == "switch") {
      return handle_switch(iss, svc, current_graph, modified, config);
    } else if (cmd == "close") {
      return handle_close(iss, svc, current_graph, modified, config);
    } else if (cmd == "print") {
      return handle_print(iss, svc, current_graph, modified, config);
    } else if (cmd == "node") {
      return handle_node(iss, svc, current_graph, modified, config);
    } else if (cmd == "ops") {
      return handle_ops(iss, svc, current_graph, modified, config);
    } else if (cmd == "traversal") {
      return handle_traversal(iss, svc, current_graph, modified, config);
    } else if (cmd == "config") {
      return handle_config(iss, svc, current_graph, modified, config);
    } else if (cmd == "read") {
      return handle_read(iss, svc, current_graph, modified, config);
    } else if (cmd == "source") {
      return handle_source(iss, svc, current_graph, modified, config);
    } else if (cmd == "output") {
      return handle_output(iss, svc, current_graph, modified, config);
    } else if (cmd == "clear-graph") {
      return handle_clear_graph(iss, svc, current_graph, modified, config);
    } else if (cmd == "clear-cache" || cmd == "cc") {
      return handle_clear_cache(iss, svc, current_graph, modified, config);
    } else if (cmd == "free") {
      return handle_free(iss, svc, current_graph, modified, config);
    } else if (cmd == "compute") {
      return handle_compute(iss, svc, current_graph, modified, config);
    } else if (cmd == "save") {
      return handle_save(iss, svc, current_graph, modified, config);
    } else if (cmd == "exit" || cmd == "quit" || cmd == "q") {
      return handle_exit(iss, svc, current_graph, modified, config);
    } else if (cmd == "bench") {
      return handle_bench(iss, svc, current_graph, modified, config);
    } else if (cmd == "benchmark") {
      return handle_benchmark(iss, svc, current_graph, modified, config);
    } else {
      std::cout << "Unknown command: " << cmd
                << ". Type 'help' for a list of commands.\n";
    }
  } catch (const std::exception& e) {
    std::cout << "Error: " << e.what() << "\n";
  }
  return true;
}
