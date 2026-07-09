#include "adapter/buffer_adapter_opencv.hpp"
#include "plugin_api.hpp"

namespace {

/**
 * @brief Maps save side-effect demand to the same upstream input ROI.
 *
 * The save plugin writes a complete file as a monolithic side effect, but ROI
 * propagation still needs an explicit upstream demand contract. A changed
 * downstream side-effect region requires the corresponding input region to be
 * current before the monolithic file write runs.
 *
 * @param node Save node being planned; the output path does not affect ROI
 * mapping.
 * @param roi Downstream side-effect ROI requested by the planner.
 * @param graph Graph containing the node; unused for pass-through mapping.
 * @return The identical upstream ROI required from the input image.
 * @throws Nothing.
 * @note Execution remains monolithic and rewrites the full file. This function
 * only prevents the plugin from relying on legacy identity fallback.
 */
cv::Rect save_dirty_roi(const ps::Node& node, const cv::Rect& roi,
                        const ps::GraphModel& graph) {
  (void)node;
  (void)graph;
  return roi;
}

/**
 * @brief Projects input image changes to the save side-effect boundary.
 *
 * @param node Save node being planned; the output path does not affect ROI
 * mapping.
 * @param roi Upstream input ROI that changed.
 * @param graph Graph containing the node; unused for pass-through mapping.
 * @param parent_size Upstream image size; unused because mapping is
 * pass-through.
 * @param child_size Save side-effect extent; unused because save has no image
 * output contract.
 * @return The input ROI as the affected save side-effect region.
 * @throws Nothing.
 * @note The returned ROI is diagnostic/planning metadata only; save execution
 * still writes the whole image to the configured path.
 */
cv::Rect save_forward_roi(const ps::Node& node, const cv::Rect& roi,
                          const ps::GraphModel& graph,
                          const cv::Size& parent_size,
                          const cv::Size& child_size) {
  (void)node;
  (void)graph;
  (void)parent_size;
  (void)child_size;
  return roi;
}

}  // namespace

/**
 * @brief Writes the input image to disk as a monolithic side effect.
 *
 * @param node Graph node containing the required `path` runtime parameter.
 * @param inputs Borrowed upstream outputs; the first input must contain a valid
 * image buffer.
 * @return Empty NodeOutput because the save plugin produces no image output.
 * @throws ps::GraphError when the input image is missing, `path` is missing, or
 * OpenCV fails to write the file.
 * @note The operation converts to 16-bit PNG-style intensity before writing.
 * It must be treated as side-effecting HP monolithic work, not as a reusable
 * image-producing operator.
 */
ps::NodeOutput op_save(const ps::Node& node,
                       const std::vector<const ps::NodeOutput*>& inputs) {
  if (inputs.empty() || inputs[0]->image_buffer.width == 0) {
    throw ps::GraphError(ps::GraphErrc::MissingDependency,
                         "Save op requires an input image.");
  }

  const auto& P = node.runtime_parameters;
  std::string path;
  if (P["path"])
    path = P["path"].as<std::string>();
  if (path.empty()) {
    throw ps::GraphError(ps::GraphErrc::InvalidParameter,
                         "Save op requires a 'path' parameter.");
  }

  cv::Mat image_to_save = ps::toCvMat(inputs[0]->image_buffer);

  cv::Mat out_mat;
  image_to_save.convertTo(out_mat, CV_16U, 65535.0);

  try {
    cv::imwrite(path, out_mat);
  } catch (const cv::Exception& ex) {
    throw ps::GraphError(ps::GraphErrc::Io,
                         "Failed to save image to " + path + ": " + ex.what());
  }

  return ps::NodeOutput();
}

/**
 * @brief Registers the save side-effect operation and explicit ROI contracts.
 *
 * @return Nothing.
 * @param registrar Host-provided registration API. The pointer is valid only
 * during this call.
 * @throws std::invalid_argument when the loader passes a null registrar.
 * @throws std::logic_error if the host registrar is incomplete.
 * @throws Exceptions from host registry allocation or callback storage may
 * propagate to the plugin loader.
 * @note Save remains an HP monolithic side-effect plugin. Explicit ROI
 * propagators document its planning contract and avoid legacy fallback while
 * registration stays on the host-owned registry.
 */
extern "C" PLUGIN_API void register_photospider_ops_v1(
    ps::OperationPluginRegistrar* registrar) {
  if (!registrar) {
    throw std::invalid_argument(
        "register_photospider_ops_v1 requires registrar");
  }
  registrar->register_op_hp_monolithic("io", "save", op_save);
  registrar->register_dirty_propagator("io", "save",
                                       ps::DirtyRoiPropFunc(save_dirty_roi));
  registrar->register_forward_propagator(
      "io", "save", ps::ForwardRoiPropFunc(save_forward_roi));
}
