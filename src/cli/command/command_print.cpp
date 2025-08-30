// FILE: src/cli/command/command_print.cpp
#include <iostream>
#include <sstream>
#include <optional>
#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

bool handle_print(std::istringstream& iss,
                  ps::InteractionService& svc,
                  std::string& current_graph,
                  bool& /*modified*/,
                  CliConfig& config) {
    if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
    std::string target_str = "all";
    std::string mode_str = config.default_print_mode;
    bool target_is_set = false;

    std::string arg;
    while(iss >> arg) {
        if (arg == "f" || arg == "full" || arg == "s" || arg == "simplified") { mode_str = arg; }
        else {
            if (target_is_set) { std::cout << "Warning: Multiple targets specified for print; using last one ('" << arg << "').\n"; }
            target_str = arg;
            target_is_set = true;
        }
    }

    bool show_params = (mode_str == "f" || mode_str == "full");
    if (target_str == "all") {
        auto dump = svc.cmd_dump_tree(current_graph, std::nullopt, show_params);
        if (dump) std::cout << *dump; else std::cout << "(failed to dump tree)\n";
    } else {
        try {
            int node_id = std::stoi(target_str);
            auto dump = svc.cmd_dump_tree(current_graph, node_id, show_params);
            if (dump) std::cout << *dump; else std::cout << "(failed to dump tree)\n";
        } catch (const std::exception&) {
            std::cout << "Error: Invalid target '" << target_str << "'. Must be an integer ID or 'all'." << std::endl;
        }
    }
    return true;
}

void print_help_print(const CliConfig& /*config*/) {
    print_help_from_file("help_print.txt");
}

