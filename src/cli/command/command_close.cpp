// FILE: src/cli/command/command_close.cpp
#include <iostream>
#include <sstream>
#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

bool handle_close(std::istringstream& iss,
                  ps::InteractionService& svc,
                  std::string& current_graph,
                  bool& /*modified*/,
                  CliConfig& /*config*/) {
    std::string name; iss >> name; if (name.empty()) { std::cout << "Usage: close <name>\n"; return true; }
    if (!svc.cmd_close_graph(name)) { std::cout << "Error: failed to close '" << name << "'.\n"; return true; }
    if (current_graph == name) current_graph.clear();
    std::cout << "Closed graph '" << name << "'.\n";
    return true;
}

void print_help_close(const CliConfig& /*config*/) {
    print_help_from_file("help_close.txt");
}

