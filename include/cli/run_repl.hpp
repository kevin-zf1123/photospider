// FILE: include/cli/run_repl.hpp
#pragma once
#include <string>

#include "cli_config.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"

void run_repl(ps::Host& svc, CliConfig& config,
              const std::string& initial_graph);
