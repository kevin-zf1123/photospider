#pragma once
#include "common/diagnostics.hpp"
#include "common/options.hpp"

namespace ps::tools {
bool interruption_requested() noexcept;
void interrupted();
void configure_diagnostics(const CommonOptions& options,
                           Diagnostics* diagnostics);
void print_common_help();
void print_version(const char* name);
int run_cli(int argc, char** argv, int (*driver)(int, char**, Diagnostics*));
}  // namespace ps::tools
