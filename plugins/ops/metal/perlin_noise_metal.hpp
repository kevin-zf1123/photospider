#pragma once

#include "graph/node.hpp"  // NOLINT(build/include_subdir)

namespace ps {
namespace ops {

/**
 * @brief Executes one Host-private Perlin noise dispatch through Apple Metal.
 *
 * @param node Operation node containing width, height, grid_size, and seed
 * parameters.
 * @return Nothing after publishing one pending CPU-replica Value into the
 * current process-owned Metal execution context.
 * @throws std::bad_alloc unchanged when CPU-side parameter, permutation,
 * readback, output allocation, or contextual diagnostic construction exhausts
 * memory.
 * @throws std::runtime_error with the current Metal stage when executor
 * context, pipeline, allocation, command, invalid-parameter, or unknown
 * execution failures occur.
 * @note Pure-C operation ABI v1 is synchronous CPU-only and intentionally has
 * no native handle or delayed completion. This function is therefore a
 * source-private adapter, not an operation-plugin callback or installed API.
 * It must run inside a process-owned Metal executor context.
 */
void execute_perlin_noise_metal(const Node& node);

}  // namespace ops
}  // namespace ps
