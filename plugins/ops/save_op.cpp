#include <opencv2/imgcodecs.hpp>
#include <stdexcept>
#include <string>

#include "photospider/core/graph_error.hpp"
#include "photospider/plugin/opencv_adapter.hpp"
#include "photospider/plugin/plugin_api.hpp"

namespace {

/**
 * @brief Maps save side-effect demand to the same upstream input ROI.
 *
 * The save plugin writes a complete file as a monolithic side effect, but ROI
 * propagation still needs an explicit upstream demand contract. A changed
 * downstream side-effect region requires the corresponding input region to be
 * current before the monolithic file write runs.
 *
 * @param context Immutable dirty ROI and topology snapshot.
 * @return The identical upstream ROI required from the input image.
 * @throws Nothing.
 * @note Execution remains monolithic and rewrites the full file. This function
 * only declares the upstream demand visible through the public plugin
 * contract.
 */
ps::PixelRect save_dirty_roi(const ps::plugin::RoiContext& context) {
  return context.requested_roi;
}

/**
 * @brief Projects input image changes to the save side-effect boundary.
 *
 * @param context Immutable forward ROI snapshot with the active input edge.
 * @return The input ROI as the affected save side-effect region.
 * @throws Nothing.
 * @note The returned ROI is diagnostic/planning metadata only; save execution
 * still writes the whole image to the configured path.
 */
ps::PixelRect save_forward_roi(const ps::plugin::RoiContext& context) {
  return context.requested_roi;
}

}  // namespace

/**
 * @brief Writes the input image to disk as a monolithic side effect.
 *
 * @param node Public node snapshot containing the required `path` effective
 * parameter.
 * @param inputs Borrowed public input views; the first input must contain a
 * valid image buffer.
 * @return Empty public operation output because save produces no image output.
 * @throws ps::GraphError when the input image is missing, `path` is missing, or
 * OpenCV fails to write the file.
 * @note The operation converts to 16-bit PNG-style intensity before writing.
 * It must be treated as side-effecting HP monolithic work, not as a reusable
 * image-producing operator.
 */
ps::plugin::OperationOutput op_save(
    const ps::plugin::NodeView& node,
    ps::plugin::ArrayView<ps::plugin::OperationInputView> inputs) {
  if (inputs.empty() || !inputs[0].image_buffer ||
      inputs[0].image_buffer->width == 0) {
    throw ps::GraphError(ps::GraphErrc::MissingDependency,
                         "Save op requires an input image.");
  }

  const auto& parameters = node.parameters();
  std::string path;
  const auto found = parameters.find("path");
  if (found != parameters.end() && found->second.is_string()) {
    path = found->second.as_string();
  }
  if (path.empty()) {
    throw ps::GraphError(ps::GraphErrc::InvalidParameter,
                         "Save op requires a 'path' parameter.");
  }

  cv::Mat image_to_save = ps::plugin::opencv::to_mat(*inputs[0].image_buffer);

  cv::Mat out_mat;
  image_to_save.convertTo(out_mat, CV_16U, 65535.0);

  try {
    if (!cv::imwrite(path, out_mat)) {
      throw ps::GraphError(ps::GraphErrc::Io,
                           "OpenCV rejected image output path: " + path);
    }
  } catch (const cv::Exception& ex) {
    throw ps::GraphError(ps::GraphErrc::Io,
                         "Failed to save image to " + path + ": " + ex.what());
  }

  return ps::plugin::OperationOutput();
}

/**
 * @brief Registers the save side-effect operation and explicit ROI contracts.
 *
 * @param registrar Host-provided registration API. The pointer is valid only
 * during this call.
 * @return Nothing.
 * @throws std::invalid_argument when the loader passes a null registrar.
 * @throws std::logic_error if the host registrar is incomplete.
 * @throws std::bad_alloc if operation names or callback storage exhausts
 * memory.
 * @note Save remains an HP monolithic side-effect plugin. Explicit ROI
 * propagators document its planning contract while registration stays on the
 * host-owned registry.
 */
extern "C" PHOTOSPIDER_OPERATION_PLUGIN_EXPORT void register_photospider_ops_v2(
    ps::plugin::OperationPluginRegistrar* registrar) {
  if (!registrar) {
    throw std::invalid_argument(
        "register_photospider_ops_v2 requires registrar");
  }
  registrar->register_op_hp_monolithic("io", "save", op_save);
  registrar->register_dirty_propagator("io", "save", save_dirty_roi);
  registrar->register_forward_propagator("io", "save", save_forward_roi);
}
