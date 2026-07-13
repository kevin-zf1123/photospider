#include <stdexcept>

#include "metal/perlin_noise_metal.hpp"
#include "photospider/plugin/plugin_api.hpp"

namespace {

/**
 * @brief Keeps Metal Perlin dirty demand in generator output coordinates.
 *
 * The Metal Perlin plugin is a generator with no upstream image dependency.
 * Returning the requested ROI makes its dirty contract explicit for planner
 * diagnostics and source-node dirty updates.
 *
 * @param context Public generator ROI snapshot; no image parents are present.
 * @return The requested generator ROI.
 * @throws Nothing.
 * @note The current Metal implementation is monolithic HP/GPU work; this ROI
 * contract is explicit metadata, not a tiled Metal execution path.
 */
ps::PixelRect perlin_metal_dirty_roi(const ps::plugin::RoiContext& context) {
  return context.requested_roi;
}

/**
 * @brief Projects Metal Perlin source changes to downstream coordinates.
 *
 * @param context Public generator ROI snapshot.
 * @return The same ROI for downstream dirty propagation.
 * @throws Nothing.
 * @note Registering this callback keeps the public contract explicit while the
 * implementation remains monolithic HP/GPU work.
 */
ps::PixelRect perlin_metal_forward_roi(const ps::plugin::RoiContext& context) {
  return context.requested_roi;
}

}  // namespace

/**
 * @brief Registers the Metal Perlin generator and explicit ROI contracts.
 *
 * @param registrar Host-provided registration API. The pointer is valid only
 * during this call.
 * @return Nothing.
 * @throws std::invalid_argument when the loader passes a null registrar.
 * @throws std::logic_error if the host registrar is incomplete.
 * @throws std::bad_alloc if registration or Metal initialization storage
 * exhausts memory.
 * @throws std::system_error if Metal one-time initialization cannot coordinate.
 * @throws std::runtime_error if Metal initialization fails.
 * @note `extern "C"` keeps the symbol name stable for dynamic plugin loading.
 * The registered op is HP monolithic GPU work; tiled Metal execution remains a
 * future capability. Registration is routed through the host-owned registry.
 */
extern "C" PHOTOSPIDER_OPERATION_PLUGIN_EXPORT void register_photospider_ops_v2(
    ps::plugin::OperationPluginRegistrar* registrar) {
  if (!registrar) {
    throw std::invalid_argument(
        "register_photospider_ops_v2 requires registrar");
  }
  ps::plugin::OperationMetadata metal_meta;
  metal_meta.device_preference = ps::Device::GPU_METAL;

  registrar->register_op_hp_monolithic("image_generator", "perlin_noise_metal",
                                       ps::ops::op_perlin_noise_metal,
                                       metal_meta);
  registrar->register_dirty_propagator("image_generator", "perlin_noise_metal",
                                       perlin_metal_dirty_roi);
  registrar->register_forward_propagator(
      "image_generator", "perlin_noise_metal", perlin_metal_forward_roi);

  ps::ops::perlin_noise_metal_eager_init();
}
