// FILE: src/cli/command/command_clear_graph.cpp
#include <iostream>
#include <sstream>
#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

bool handle_clear_graph(std::istringstream& /*iss*/,
                        ps::InteractionService& svc,
                        std::string& current_graph,
                        bool& modified,
                        CliConfig& /*config*/) {
    if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
    svc.cmd_clear_graph(current_graph); modified = true; std::cout << "Graph cleared.\n";
    return true;
}

void print_help_clear_graph(const CliConfig& /*config*/) {
    print_help_from_file("help_clear-graph.txt");
}

