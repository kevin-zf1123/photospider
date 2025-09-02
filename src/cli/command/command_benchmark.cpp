#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "cli/benchmark_config_editor.hpp"
#include <iostream>

bool handle_benchmark(std::istringstream& iss,
                      ps::InteractionService& svc, // <-- svc is already here
                      std::string& /*current_graph*/,
                      bool& /*modified*/,
                      CliConfig& /*config*/) {
    std::string benchmark_dir;
    iss >> benchmark_dir;
    if (benchmark_dir.empty()) {
        print_help_benchmark({});
        return true;
    }
    
    // FIX: Pass the interaction service to the editor
    ps::run_benchmark_config_editor(svc, benchmark_dir);
    
    return true;
}

void print_help_benchmark(const CliConfig& /*config*/) {
    print_help_from_file("help_benchmark.txt");
}