#include "metal/perlin_noise_metal.hpp"
#include "plugin_api.hpp"
#include "ps_types.hpp"

namespace {

/**
 * @brief Keeps Metal Perlin dirty demand in generator output coordinates.
 *
 * The Metal Perlin plugin is a generator with no upstream image dependency.
 * Returning the requested ROI makes its dirty contract explicit for planner
 * diagnostics and source-node dirty updates.
 *
 * @param node Generator node being planned; Perlin parameters do not change ROI
 * coordinate space.
 * @param roi Requested generator output ROI.
 * @param graph Graph containing the node; unused because there are no image
 * parents to inspect.
 * @return The requested generator ROI.
 * @throws Nothing.
 * @note The current Metal implementation is monolithic HP/GPU work; this ROI
 * contract is explicit metadata, not a tiled Metal execution path.
 */
cv::Rect perlin_metal_dirty_roi(const ps::Node& node, const cv::Rect& roi,
                                const ps::GraphModel& graph) {
  (void)node;
  (void)graph;
  return roi;
}

/**
 * @brief Projects Metal Perlin source changes to downstream coordinates.
 *
 * @param node Generator node being planned; Perlin parameters do not change ROI
 * coordinate space.
 * @param roi Generator output ROI that changed.
 * @param graph Graph containing the node; unused because the mapping is local.
 * @param parent_size Generator output size; unused for identity projection.
 * @param child_size Downstream size; unused for identity projection.
 * @return The same ROI for downstream dirty propagation.
 * @throws Nothing.
 * @note This prevents the Metal loader from relying on legacy identity
 * fallback while keeping the plugin documented as monolithic HP/GPU work.
 */
cv::Rect perlin_metal_forward_roi(const ps::Node& node, const cv::Rect& roi,
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
 * @brief Registers the Metal Perlin generator and explicit ROI contracts.
 *
 * @return Nothing.
 * @param registrar Host-provided registration API. The pointer is valid only
 * during this call.
 * @throws std::invalid_argument when the loader passes a null registrar.
 * @throws std::logic_error if the host registrar is incomplete.
 * @throws Exceptions from host registry allocation, Metal eager
 * initialization, or callback storage may propagate to the plugin loader.
 * @note `extern "C"` keeps the symbol name stable for dynamic plugin loading.
 * The registered op is HP monolithic GPU work; tiled Metal execution remains a
 * future capability. Registration is routed through the host-owned registry.
 */
extern "C" PLUGIN_API void register_photospider_ops_v1(
    ps::OperationPluginRegistrar* registrar) {
  if (!registrar) {
    throw std::invalid_argument(
        "register_photospider_ops_v1 requires registrar");
  }
  ps::OpMetadata metal_meta;
  metal_meta.device_preference = ps::Device::GPU_METAL;

  registrar->register_op_hp_monolithic("image_generator", "perlin_noise_metal",
                                       ps::ops::op_perlin_noise_metal,
                                       metal_meta);
  registrar->register_dirty_propagator(
      "image_generator", "perlin_noise_metal",
      ps::DirtyRoiPropFunc(perlin_metal_dirty_roi));
  registrar->register_forward_propagator(
      "image_generator", "perlin_noise_metal",
      ps::ForwardRoiPropFunc(perlin_metal_forward_roi));

  perlin_noise_metal_eager_init();
}
