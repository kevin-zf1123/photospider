// FILE: include/cli/run_repl.hpp
#pragma once
#include <string>

#include "cli_config.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"

/**
 * @brief Runs the interactive command loop against a product Host.
 *
 * The REPL owns terminal/history/completion state, forwards complete lines to
 * process_command(), updates the active session label, and restores terminal
 * mode around command execution and normal loop exit.
 *
 * @param svc Borrowed Host used synchronously by completion and commands.
 * @param config Mutable CLI configuration shared by command handlers.
 * @param initial_graph Initial active Host session label, which may be empty.
 * @return Nothing after an exit command or a confirmed empty-line Ctrl+C.
 * @throws std::bad_alloc if terminal, history, completion, or command state
 * cannot allocate.
 * @note The caller must keep svc and config alive for the full loop. The REPL
 * owns no Host graph/cache resources and retains no background callbacks.
 */
void run_repl(ps::Host& svc, CliConfig& config,
              const std::string& initial_graph);
