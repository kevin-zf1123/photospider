#pragma once

#include <vector>

#include "ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps {
namespace ops {

/**
 * @brief Generates one Perlin noise image through the Apple Metal backend.
 *
 * @param node Operation node containing width, height, grid_size, and seed
 * parameters.
 * @param inputs Unused upstream outputs; Perlin noise is a source operation.
 * @return NodeOutput containing a single-channel floating-point image.
 * @throws std::bad_alloc unchanged when CPU-side parameter, permutation,
 * readback, output allocation, or contextual diagnostic construction exhausts
 * memory.
 * @throws std::runtime_error with the current Metal stage when device, shader,
 * buffer, command, invalid-parameter, lock, or unknown execution failures
 * occur.
 * @note Calls acquire their process-wide serialization mutex inside the same
 * contextual exception boundary as Metal execution. Returned image storage
 * owns its CPU copy independently of temporary Metal resources.
 */
NodeOutput op_perlin_noise_metal(const Node& node,
                                 const std::vector<const NodeOutput*>& inputs);

}  // namespace ops
}  // namespace ps

/**
 * @brief Eagerly initializes the process-wide Metal Perlin state.
 *
 * @return Nothing.
 * @throws std::bad_alloc unchanged when state allocation exhausts memory.
 * @throws std::system_error when std::call_once cannot coordinate state
 * initialization.
 * @throws std::runtime_error when Metal device, queue, shader, or pipeline
 * creation fails.
 * @note Initialization is process-wide and guarded by std::call_once.
 */
extern "C" void perlin_noise_metal_eager_init();
