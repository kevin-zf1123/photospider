#include <stdexcept>

#include "photospider/core/graph_error.hpp"
#include "photospider/plugin/opencv_adapter.hpp"
#include "photospider/plugin/plugin_api.hpp"

namespace {

/**
 * @brief Maps invert dirty demand directly to its input ROI.
 *
 * Invert is a pointwise operation: each output pixel depends only on the input
 * pixel at the same coordinate. The dirty planner can therefore use the
 * requested downstream ROI as the upstream ROI without halo expansion.
 *
 * @param context Immutable ROI request and topology snapshot.
 * @return The identical upstream ROI required from the single image input.
 * @throws Nothing.
 * @note The returned rectangle is intentionally not clipped here; graph extent
 * resolution and planner clipping remain owned by the core services.
 */
ps::PixelRect invert_dirty_roi(const ps::plugin::RoiContext& context) {
  return context.requested_roi;
}

/**
 * @brief Projects an upstream invert ROI to the same output coordinates.
 *
 * @param context Immutable ROI request whose active_edge identifies the input.
 * @return The identical output ROI affected by the upstream change.
 * @throws Nothing.
 * @note Registering this callback makes the pointwise mapping explicit in the
 * public plugin contract.
 */
ps::PixelRect invert_forward_roi(const ps::plugin::RoiContext& context) {
  return context.requested_roi;
}

}  // namespace

/**
 * @brief Executes the pointwise image inversion plugin operation.
 *
 * @param node Public node view; unused by this parameter-free operation.
 * @param inputs Borrowed upstream input snapshots.
 * @return Owned operation output containing the inverted image buffer.
 * @throws ps::GraphError when the required input image is missing.
 * @throws cv::Exception if OpenCV subtraction or buffer conversion fails.
 * @note The operation is monolithic HP work. It uses callback-local CPU
 * `cv::Mat` values, reads borrowed inputs, returns a newly wrapped output
 * buffer, owns no shared provider state, and is reentrant without mutating
 * graph state.
 */
ps::plugin::OperationOutput op_invert(
    const ps::plugin::NodeView& node,
    ps::plugin::ArrayView<ps::plugin::OperationInputView> inputs) {
  (void)node;
  if (inputs.empty() || !inputs[0].image_buffer ||
      inputs[0].image_buffer->width == 0) {
    throw ps::GraphError(ps::GraphErrc::MissingDependency,
                         "Invert op requires one valid input image.");
  }

  const cv::Mat input = ps::plugin::opencv::to_mat(*inputs[0].image_buffer);

  cv::Mat output;
  cv::subtract(cv::Scalar::all(1.0), input, output);

  ps::plugin::OperationOutput result;
  result.image_buffer = ps::plugin::opencv::from_mat(output);
  return result;
}

/**
 * @brief Registers the invert operation and its explicit ROI contracts.
 *
 * @param registrar Host-provided registration API. The pointer is valid only
 * during this call.
 * @return Nothing.
 * @throws std::invalid_argument when the loader passes a null registrar.
 * @throws std::logic_error if the host registrar is incomplete.
 * @throws std::bad_alloc if operation names or callback storage exhausts
 * memory.
 * @note The callback stores only public SDK values and receives all registry
 * services through the host-provided registrar.
 */
extern "C" PHOTOSPIDER_OPERATION_PLUGIN_EXPORT void register_photospider_ops_v2(
    ps::plugin::OperationPluginRegistrar* registrar) {
  if (!registrar) {
    throw std::invalid_argument(
        "register_photospider_ops_v2 requires registrar");
  }
  registrar->register_op_hp_monolithic("image_process", "invert", op_invert);
  registrar->register_dirty_propagator("image_process", "invert",
                                       invert_dirty_roi);
  registrar->register_forward_propagator("image_process", "invert",
                                         invert_forward_roi);
}
