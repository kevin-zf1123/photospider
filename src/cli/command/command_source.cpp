// FILE: src/cli/command/command_source.cpp
#include <fstream>
#include <iostream>
#include <sstream>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "cli/process_command.hpp"

bool handle_source(std::istringstream& iss, ps::InteractionService& svc,
                   std::string& current_graph, bool& modified,
                   CliConfig& config) {
  std::string filename;
  iss >> filename;
  if (filename.empty()) {
    std::cout << "Usage: source <filename>\n";
    return true;
  }
  std::ifstream script_file(filename);
  if (!script_file) {
    std::cout << "Error: Cannot open script file: " << filename << "\n";
    return true;
  }
  std::string script_line;
  while (std::getline(script_file, script_line)) {
    if (script_line.empty() ||
        script_line.find_first_not_of(" \t") == std::string::npos ||
        script_line[0] == '#')
      continue;
    std::cout << "ps> " << script_line << std::endl;
    if (!process_command(script_line, svc, current_graph, modified, config))
      return false;
  }
  return true;
}

void print_help_source(const CliConfig& /*config*/) {
  print_help_from_file("help_source.txt");
}
