// FILE: apps/graph_cli/src/command/help_utils.cpp
#include "graph_cli/command/help_utils.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <string>

namespace fs = std::filesystem;

/** @copydoc print_help_from_file */
void print_help_from_file(const std::string& filename) {
  try {
    const fs::path path = fs::path(PHOTOSPIDER_GRAPH_CLI_HELP_DIR) / filename;
    std::ifstream in(path);
    if (!in) {
      std::cout << "(Help not available: " << path.string() << ")\n";
      return;
    }
    std::string line;
    while (std::getline(in, line)) {
      std::cout << line << '\n';
    }
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& e) {
    std::cout << "(Error reading help file: " << e.what() << ")\n";
  }
}
