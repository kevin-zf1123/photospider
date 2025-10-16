// FILE: include/cli/process_command.hpp
#pragma once
#include <string>

#include "cli_config.hpp"
#include "kernel/interaction.hpp"

// Returns whether to continue REPL (false means exit)
bool process_command(const std::string& line, ps::InteractionService& svc,
                     std::string& current_graph, bool& modified,
                     CliConfig& config);
