// CLI Config Editor facade (front-end only)
#pragma once

#include "cli_config.hpp"

// Launch the TUI config editor. This is a pure front-end feature; the kernel
// does not manage user config files.
void run_config_editor(CliConfig& config);
