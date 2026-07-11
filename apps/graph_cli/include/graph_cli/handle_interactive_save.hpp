// FILE: apps/graph_cli/include/graph_cli/handle_interactive_save.hpp
#pragma once

#include "graph_cli/cli_config.hpp"

/**
 * @brief Applies the configured save policy after interactive editing.
 *
 * With `ask`, prompts for confirmation and, when no configuration path was
 * loaded, prompts for a destination that defaults to `config.yaml`. With
 * `auto_save_on_apply`, writes to the loaded path or prints a warning when no
 * path is available. Other policy values perform no action. A false result
 * from the configuration serializer is not surfaced by this wrapper.
 *
 * @param config Borrowed configuration whose save policy, loaded path, and
 * serializable fields are read; this function does not modify it.
 * @return Nothing after the selected prompt and write flow completes.
 * @throws std::bad_alloc If prompt, response, path, YAML, or stream storage
 * cannot allocate.
 * @throws std::ios_base::failure If `std::cin` or `std::cout` is configured to
 * throw after an input or output failure.
 * @note The call is synchronous, retains no references, and may read standard
 * input, write standard output, and create or truncate one YAML file. The
 * caller must keep `config` alive and serialize concurrent access to it, the
 * global streams, and the destination file.
 */
void handle_interactive_save(CliConfig& config);
