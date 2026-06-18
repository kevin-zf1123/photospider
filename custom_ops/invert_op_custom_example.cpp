#include "adapter/buffer_adapter_opencv.hpp"
#include "plugin_api.hpp"

namespace {

/**
 * @brief Maps invert dirty demand directly to its input ROI.
 *
 * Invert is a pointwise operation: each output pixel depends only on the input
 * pixel at the same coordinate. The dirty planner can therefore use the
 * requested downstream ROI as the upstream ROI without halo expansion.
 *
 * @param node Invert node being planned; unused because the mapping is
 * parameter-independent.
 * @param roi Downstream ROI requested from the invert output.
 * @param graph Graph containing the node; unused for pointwise mapping.
 * @return The identical upstream ROI required from the single image input.
 * @throws Nothing.
 * @note The returned rectangle is intentionally not clipped here; graph extent
 * resolution and planner clipping remain owned by the core services.
 */
cv::Rect invert_dirty_roi(const ps::Node& node, const cv::Rect& roi,
                          const ps::GraphModel& graph) {
  (void)node;
  (void)graph;
  return roi;
}

/**
 * @brief Projects an upstream invert ROI to the same output coordinates.
 *
 * @param node Invert node being planned; unused because the mapping is
 * parameter-independent.
 * @param roi Upstream input ROI that changed.
 * @param graph Graph containing the node; unused for pointwise mapping.
 * @param parent_size Upstream image size; unused because the mapping is
 * one-to-one.
 * @param child_size Output image size; unused because the mapping is
 * one-to-one.
 * @return The identical output ROI affected by the upstream change.
 * @throws Nothing.
 * @note This explicit contract prevents the plugin from relying on legacy
 * identity fallback.
 */
cv::Rect invert_forward_roi(const ps::Node& node, const cv::Rect& roi,
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
 * @brief Executes the pointwise image inversion plugin operation.
 *
 * @param node Graph node being executed; current implementation does not read
 * node parameters.
 * @param inputs Borrowed upstream outputs; the first input must contain a valid
 * image buffer.
 * @return NodeOutput containing the inverted image buffer.
 * @throws ps::GraphError when the required input image is missing.
 * @throws cv::Exception if OpenCV subtraction or buffer conversion fails.
 * @note The operation is monolithic HP work. It reads borrowed inputs and
 * returns a newly wrapped output buffer without mutating graph state.
 */
ps::NodeOutput op_invert(const ps::Node& node,
                         const std::vector<const ps::NodeOutput*>& inputs) {
  (void)node;
  if (inputs.empty() || inputs[0]->image_buffer.width == 0) {
    throw ps::GraphError(ps::GraphErrc::MissingDependency,
                         "Invert op requires one valid input image.");
  }

  const cv::UMat& u_input = ps::toCvUMat(inputs[0]->image_buffer);

  cv::UMat u_output;
  cv::subtract(cv::Scalar::all(1.0), u_input, u_output);

  ps::NodeOutput result;
  result.image_buffer = ps::fromCvUMat(u_output);
  return result;
}

/**
 * @brief Registers the invert operation and its explicit ROI contracts.
 *
 * @return Nothing.
 * @throws Exceptions from OpRegistry allocation or callback storage may
 * propagate to the plugin loader.
 * @note The plugin uses current HP monolithic registration plus explicit
 * dirty/forward propagators instead of depending on legacy identity fallback.
 */
extern "C" PLUGIN_API void register_photospider_ops() {
  auto& registry = ps::OpRegistry::instance();
  registry.register_op_hp_monolithic("image_process", "invert", op_invert);
  registry.register_dirty_propagator("image_process", "invert",
                                     ps::DirtyRoiPropFunc(invert_dirty_roi));
  registry.register_forward_propagator(
      "image_process", "invert", ps::ForwardRoiPropFunc(invert_forward_roi));
}
