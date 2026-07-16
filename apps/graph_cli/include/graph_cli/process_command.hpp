// FILE: apps/graph_cli/include/graph_cli/process_command.hpp
#pragma once
#include <new>
#include <string>

#include "graph_cli/cli_config.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"

/**
 * @brief Dispatches one parsed line to the matching CLI command handler.
 *
 * Execution first extracts the command token, then invokes exactly one
 * matching handler with borrowed CLI state, and finally translates ordinary
 * standard failures into terminal diagnostics while preserving resource
 * exhaustion.
 *
 * @param line Complete command line supplied by the REPL or a script.
 * @param svc Product Host used by command handlers.
 * @param current_graph Mutable current-session label maintained by the CLI.
 * @param modified Mutable graph-modification flag maintained by the CLI.
 * @param config Mutable CLI configuration shared by command handlers.
 * @return True to continue the REPL, or false after an exit command.
 * @throws std::bad_alloc unchanged when parsing, dispatch, Host work, or
 * diagnostic output exhausts memory.
 * @note Other standard exceptions are reported as recoverable command errors.
 * The function borrows all referenced state only for the duration of the call.
 */
bool process_command(const std::string& line, ps::Host& svc,
                     std::string& current_graph, bool& modified,
                     CliConfig& config);
