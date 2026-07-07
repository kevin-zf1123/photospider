// FILE: src/cli/command/command_save.cpp
#include <iostream>
#include <sstream>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "cli/save_fp32_image.hpp"

bool handle_save(std::istringstream& iss, ps::InteractionService& svc,
                 std::string& current_graph, bool& /*modified*/,
                 CliConfig& config) {
  if (current_graph.empty()) {
    std::cout << "No current graph. Use load/switch.\n";
    return true;
  }
  int node_id = -1;
  iss >> node_id;
  if (node_id < 0) {
    std::cout << "Usage: save <id> <file>\n";
    return true;
  }
  std::string path;
  iss >> path;
  if (path.empty()) {
    std::cout << "Usage: save <id> <file>\n";
    return true;
  }

  ps::Kernel::ComputeRequest request;
  request.name = current_graph;
  request.node_id = node_id;
  request.cache.precision = config.cache_precision;
  request.cache.force_recache = false;
  request.cache.disable_disk_cache = false;
  request.telemetry.enable_timing = false;
  request.execution.parallel = false;
  auto image = svc.cmd_compute_and_get_image(request);
  if (!image || image->empty()) {
    std::cout << "No image to save (node produced no image).\n";
    return true;
  }
  if (save_fp32_image(*image, path, config)) {
    std::cout << "Saved image to " << path << "\n";
  } else {
    std::cout << "Failed to save image to " << path << "\n";
  }
  return true;
}

void print_help_save(const CliConfig& /*config*/) {
  print_help_from_file("help_save.txt");
}
