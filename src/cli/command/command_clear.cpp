// FILE: src/cli/command/command_clear.cpp
#include <iostream>
#include <sstream>
#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

bool handle_clear(std::istringstream& /*iss*/,
                  ps::InteractionService& /*svc*/,
                  std::string& /*current_graph*/,
                  bool& /*modified*/,
                  CliConfig& /*config*/) {
    std::cout << "\033[2J\033[1;1H";
    return true;
}

void print_help_clear(const CliConfig& /*config*/) {
    print_help_from_file("help_clear.txt");
}

