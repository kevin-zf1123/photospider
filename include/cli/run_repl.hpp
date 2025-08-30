// FILE: include/cli/run_repl.hpp
#pragma once
#include <string>
#include "cli_config.hpp"
#include "kernel/interaction.hpp"

void run_repl(ps::InteractionService& svc, CliConfig& config, const std::string& initial_graph);

