// FILE: include/cli/command/help_utils.hpp
#pragma once
#include <string>

/**
 * @brief Prints maintained command help from the CLI help directory.
 * @param filename Basename of the help file to render.
 * @return Nothing.
 * @throws std::bad_alloc if path, stream, line, or output storage cannot
 * allocate.
 * @note Missing files and ordinary I/O failures render a fallback message;
 * resource exhaustion is never converted into presentation output.
 */
void print_help_from_file(const std::string& filename);
