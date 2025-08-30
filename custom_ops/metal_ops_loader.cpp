#include "plugin_api.hpp"
// --- FIX: The path is now relative to the 'src' directory ---
#include "metal/perlin_noise_metal.hpp"

extern "C" PLUGIN_API void register_photospider_ops() {
    ps::OpRegistry::instance().register_op("image_generator", "perlin_noise_metal", ps::ops::op_perlin_noise_metal);
}
