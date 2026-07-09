// FILE: include/cli/process_command.hpp
#pragma once
#include <string>

#include "cli_config.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"

// Returns whether to continue REPL (false means exit)
bool process_command(const std::string& line, ps::Host& svc,
                     std::string& current_graph, bool& modified,
                     CliConfig& config);
