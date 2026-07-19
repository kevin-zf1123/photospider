#include <algorithm>
#include <cstddef>

#include "photospider/plugin/plugin_api.hpp"

namespace {

/**
 * @brief Produces a deterministic stdlib-only replacement for resize.
 *
 * @param node Callback-scoped replacement node identity.
 * @param inputs Borrowed upstream inputs, intentionally ignored by this test
 *        provider.
 * @return Owned 3-by-2 float32 image filled with the sentinel value `0.625`.
 * @throws std::bad_alloc if aligned image allocation fails.
 * @throws std::invalid_argument or std::overflow_error if the public image
 *         factory rejects the fixture's fixed descriptor.
 * @note The callback uses only the public operation SDK and standard library;
 *       it neither includes nor links OpenCV.
 */
ps::plugin::OperationOutput replacement_resize(
    const ps::plugin::NodeView& node,
    ps::plugin::ArrayView<ps::plugin::OperationInputView> inputs) {
  (void)node;
  (void)inputs;
  ps::plugin::OperationOutput output;
  output.image_buffer =
      ps::make_aligned_cpu_image_buffer(3, 2, 1, ps::DataType::FLOAT32);
  auto* base = static_cast<std::byte*>(output.image_buffer.data.get());
  for (int row_index = 0; row_index < output.image_buffer.height; ++row_index) {
    auto* row = reinterpret_cast<float*>(
        base + static_cast<std::size_t>(row_index) * output.image_buffer.step);
    std::fill(row, row + output.image_buffer.width, 0.625F);
  }
  output.debug.compute_device = "STDLIB_RESIZE_REPLACEMENT";
  return output;
}

/**
 * @brief Preserves replacement resize dirty demand for ownership testing.
 *
 * @param context Callback-scoped ROI snapshot.
 * @return Unchanged requested ROI.
 * @throws Nothing.
 * @note Registering this slot together with execution and forward propagation
 *       gives the replacement provider complete active ownership of the key.
 */
ps::PixelRect replacement_dirty_roi(
    const ps::plugin::RoiContext& context) noexcept {
  return context.requested_roi;
}

/**
 * @brief Preserves replacement resize forward demand for ownership testing.
 *
 * @param context Callback-scoped ROI snapshot.
 * @return Unchanged requested ROI.
 * @throws Nothing.
 * @note The fixture validates provider replacement mechanics, not resize
 *       geometry semantics.
 */
ps::PixelRect replacement_forward_roi(
    const ps::plugin::RoiContext& context) noexcept {
  return context.requested_roi;
}

}  // namespace

/**
 * @brief Registers the stdlib-only resize replacement provider.
 *
 * @param registrar Borrowed host registration transaction.
 * @return Nothing.
 * @throws std::invalid_argument, std::logic_error, std::bad_alloc, or host
 *         registration exceptions unchanged.
 * @note The registrar is callback-scoped and is never retained.
 */
extern "C" PHOTOSPIDER_OPERATION_PLUGIN_EXPORT void register_photospider_ops_v2(
    ps::plugin::OperationPluginRegistrar* registrar) {
  registrar->register_op_hp_monolithic("image_process", "resize",
                                       replacement_resize);
  registrar->register_dirty_propagator("image_process", "resize",
                                       replacement_dirty_roi);
  registrar->register_forward_propagator("image_process", "resize",
                                         replacement_forward_roi);
}
