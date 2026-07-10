#include <iostream>
#include <sstream>
#include <string>

#include "cli/benchmark_config_editor.hpp"
#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

/** @copydoc handle_benchmark */
bool handle_benchmark(std::istringstream& iss, ps::Host& svc,
                      std::string& /*current_graph*/, bool& /*modified*/,
                      CliConfig& /*config*/) {
  std::string benchmark_dir;
  iss >> benchmark_dir;
  if (benchmark_dir.empty()) {
    print_help_benchmark({});
    return true;
  }

  ps::run_benchmark_config_editor(svc, benchmark_dir);

  return true;
}

/** @copydoc print_help_benchmark */
void print_help_benchmark(const CliConfig& /*config*/) {
  print_help_from_file("help_benchmark.txt");
}
