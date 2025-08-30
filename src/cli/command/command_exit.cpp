// FILE: src/cli/command/command_exit.cpp
#include <sstream>
#include <iostream>
#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "cli/ask_yesno.hpp"

bool handle_exit(std::istringstream& /*iss*/,
                 ps::InteractionService& svc,
                 std::string& current_graph,
                 bool& /*modified*/,
                 CliConfig& config) {
    if (!current_graph.empty() && ask_yesno("Synchronize disk cache with memory state before exiting?", config.exit_prompt_sync)) {
        svc.cmd_synchronize_disk_cache(current_graph, config.cache_precision);
    }
    return false; // signal REPL exit
}

void print_help_exit(const CliConfig& /*config*/) {
    print_help_from_file("help_exit.txt");
}

