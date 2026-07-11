// FILE: apps/graph_cli/include/graph_cli/print_cli_help.hpp
#pragma once

/**
 * @brief Prints top-level command-line usage and option help to standard
 * output.
 *
 * Emits the fixed `graph_cli` invocation syntax and supported option list, then
 * terminates the help block with `std::endl` so the stream is flushed.
 *
 * @return Nothing after the complete help block is emitted and flushed.
 * @throws std::bad_alloc If standard-stream or locale storage cannot allocate.
 * @throws std::ios_base::failure If `std::cout` is configured to throw after an
 * output or flush failure.
 * @note This is a synchronous standard-output side effect; it reads no files,
 * retains no state, and starts no background work. Concurrent writers must
 * serialize access to avoid interleaved help text.
 */
void print_cli_help();
