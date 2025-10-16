// FILE: src/cli/print_cli_help.cpp
#include "cli/print_cli_help.hpp"

#include <iostream>

void print_cli_help() {
  std::cout
      << "Usage: graph_cli [options]\n\n"
      << "Options:\n"
      << "  -h, --help                 Show this help message\n"
      << "  -r, --read <file>          Read YAML and create graph\n"
      << "  -o, --output <file>        Save current graph to YAML\n"
      << "  -p, --print                Print dependency tree\n"
      << "  -t, --traversal            Show evaluation order\n"
      << "      --config <file>        Use a specific configuration file\n"
      << "      --clear-cache          Delete the contents of the cache "
         "directory\n"
      << "      --repl                 Start interactive shell (REPL)\n"
      << std::endl;
}
