// FILE: src/cli/command/command_save.cpp
#include <iostream>
#include <sstream>
#include <string>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "cli/save_fp32_image.hpp"

namespace {

/**
 * @brief Maps a public Host image data type to an OpenCV matrix depth.
 *
 * @param type Host image scalar data type copied from ImageBuffer metadata.
 * @return OpenCV depth constant compatible with CV_MAKETYPE.
 * @throws Nothing.
 * @note Unknown future enum values fall back to CV_32F through the defensive
 *       return after the switch; current DataType values are handled
 *       explicitly.
 */
int cv_depth_for(ps::DataType type) {
  switch (type) {
    case ps::DataType::UINT8:
      return CV_8U;
    case ps::DataType::INT8:
      return CV_8S;
    case ps::DataType::UINT16:
      return CV_16U;
    case ps::DataType::INT16:
      return CV_16S;
    case ps::DataType::FLOAT32:
      return CV_32F;
    case ps::DataType::FLOAT64:
      return CV_64F;
  }
  return CV_32F;
}

/**
 * @brief Creates a non-owning cv::Mat view over a Host CPU image buffer.
 *
 * @param image ImageBuffer descriptor returned by Host compute.
 * @return cv::Mat view over image.data, or an empty matrix when the descriptor
 *         is not a valid CPU image.
 * @throws Nothing directly.
 * @note The returned matrix borrows image.data; callers must keep the
 *       ImageBuffer alive while using the cv::Mat view.
 */
cv::Mat cpu_image_buffer_view(const ps::ImageBuffer& image) {
  if (!image.data || image.device != ps::Device::CPU || image.width <= 0 ||
      image.height <= 0 || image.channels <= 0) {
    return {};
  }
  return cv::Mat(image.height, image.width,
                 CV_MAKETYPE(cv_depth_for(image.type), image.channels),
                 image.data.get(), image.step);
}

}  // namespace

/**
 * @brief Handles the REPL `save` command through the Host image-compute API.
 *
 * @param iss Parsed command stream containing `<node_id> <file>`.
 * @param svc Host boundary used to compute and retrieve the requested image.
 * @param current_graph Current graph session label.
 * @param config CLI cache precision and image save configuration.
 * @return Always true so the REPL continues after command handling.
 * @throws Nothing directly; Host and save failures are reported as user-facing
 *         CLI text.
 * @note Compute failures are distinct from successful computes that produce no
 *       CPU image. Failure status messages are surfaced so missing sessions,
 *       missing nodes, and operation errors are not hidden as normal no-output
 *       cases.
 */
bool handle_save(std::istringstream& iss, ps::Host& svc,
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

  ps::HostComputeRequest request;
  request.session = ps::GraphSessionId{current_graph};
  request.node = ps::NodeId{node_id};
  request.cache.precision = config.cache_precision;
  request.cache.force_recache = false;
  request.cache.disable_disk_cache = false;
  request.telemetry.enable_timing = false;
  request.execution.parallel = false;
  auto image = svc.compute_and_get_image(request);
  if (!image.status.ok) {
    std::cout << "Failed to compute image for node " << node_id << ".\n";
    if (!image.status.message.empty()) {
      std::cout << "Reason: " << image.status.message << "\n";
    }
    return true;
  }
  cv::Mat image_view = cpu_image_buffer_view(image.value);
  if (image_view.empty()) {
    std::cout << "No image to save (node produced no CPU image).\n";
    return true;
  }
  if (save_fp32_image(image_view, path, config)) {
    std::cout << "Saved image to " << path << "\n";
  } else {
    std::cout << "Failed to save image to " << path << "\n";
  }
  return true;
}

void print_help_save(const CliConfig& /*config*/) {
  print_help_from_file("help_save.txt");
}
