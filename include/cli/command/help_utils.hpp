// FILE: include/cli/command/help_utils.hpp
#pragma once
#include <string>

#include "cli_config.hpp"

// Print help text from src/cli/command/help/<filename>.
// If the file cannot be opened, prints a default message.
void print_help_from_file(const std::string& filename);
