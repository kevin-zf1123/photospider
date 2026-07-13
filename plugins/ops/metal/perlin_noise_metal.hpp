#pragma once

#include "photospider/plugin/op_contract.hpp"

namespace ps {
namespace ops {

/**
 * @brief Generates one Perlin noise image through the Apple Metal backend.
 *
 * @param node Operation node containing width, height, grid_size, and seed
 * parameters.
 * @param inputs Unused borrowed public inputs; Perlin noise is a source
 * operation.
 * @return Public output containing a single-channel floating-point image.
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
plugin::OperationOutput op_perlin_noise_metal(
    const plugin::NodeView& node,
    plugin::ArrayView<plugin::OperationInputView> inputs);

/**
 * @brief Eagerly initializes the DSO-private Metal Perlin state.
 *
 * @return Nothing.
 * @throws std::bad_alloc unchanged when state allocation exhausts memory.
 * @throws std::system_error when std::call_once cannot coordinate state
 * initialization.
 * @throws std::runtime_error when Metal device, queue, shader, or pipeline
 * creation fails.
 * @note This ordinary C++ symbol is shared only by the two implementation
 * translation units in the same plugin DSO. It is not an operation plugin ABI
 * entry point; only `register_photospider_ops_v2` is exported for host lookup.
 */
void perlin_noise_metal_eager_init();

}  // namespace ops
}  // namespace ps
