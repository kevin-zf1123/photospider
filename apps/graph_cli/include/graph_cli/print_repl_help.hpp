// FILE: apps/graph_cli/include/graph_cli/print_repl_help.hpp
#pragma once

#include "graph_cli/cli_config.hpp"

/**
 * @brief Prints REPL command help and configured defaults to standard output.
 *
 * Emits the maintained command list and interpolates the configured cache-clear
 * default, compute flags, timer-log path, and exit synchronization prompt
 * default into the corresponding help entries.
 *
 * @param config Borrowed CLI configuration read only while rendering defaults.
 * @return Nothing after the complete REPL help block is emitted.
 * @throws std::bad_alloc If standard-stream, locale, or formatting storage
 * cannot allocate.
 * @throws std::ios_base::failure If `std::cout` is configured to throw after an
 * output failure.
 * @note The caller must keep `config` alive for this synchronous call and
 * serialize concurrent access to it and standard output. The function retains
 * no references, accesses no files or Host state, and starts no background
 * work.
 */
void print_repl_help(const CliConfig& config);
