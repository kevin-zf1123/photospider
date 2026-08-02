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
 * @throws std::runtime_error with the current Metal stage when executor
 * context, pipeline, allocation, command, invalid-parameter, or unknown
 * execution failures occur.
 * @note The operation must run inside a process-owned Metal executor context.
 * Its returned image owns a CPU copy independently of invocation resources.
 */
plugin::OperationOutput op_perlin_noise_metal(
    const plugin::NodeView& node,
    plugin::ArrayView<plugin::OperationInputView> inputs);

}  // namespace ops
}  // namespace ps
