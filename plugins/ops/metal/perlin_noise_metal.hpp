#pragma once

#include "graph/node.hpp"  // NOLINT(build/include_subdir)

namespace ps {
namespace ops {

/**
 * @brief Executes one Host-private Perlin noise dispatch through Apple Metal.
 *
 * @param node Operation node containing width, height, grid_size, and seed
 * parameters.
 * @return Nothing after publishing one pending FP32 Normalized `[0,1]`
 * CPU-replica Value into the current process-owned Metal execution context.
 * @throws std::bad_alloc unchanged when CPU-side parameter, permutation,
 * readback, output allocation, or contextual diagnostic construction exhausts
 * memory.
 * @throws std::runtime_error with the current Metal stage when executor
 * context, pipeline, allocation, command, invalid-parameter, or unknown
 * execution failures occur.
 * @note Pure-C operation ABI v1 is synchronous CPU-only and intentionally has
 * no native handle or delayed completion. It explicitly supplies the Perlin
 * sample-domain contract to the executor; R32Float storage does not imply that
 * meaning. This function is therefore a source-private adapter, not an
 * operation-plugin callback or installed API. It must run inside a process-
 * owned Metal executor context.
 */
void execute_perlin_noise_metal(const Node& node);

/**
 * @brief Publishes the build-configured Host-private Metal Perlin operation.
 *
 * Registration installs one high-precision monolithic Metal candidate with
 * exact identity propagation callbacks. Its execution wrapper borrows the
 * process-owned Metal context, collects the pending CPU-replica Value published
 * by `execute_perlin_noise_metal()`, and returns that Value through
 * `NodeOutput`.
 *
 * @return Nothing after atomically replacing this configured candidate set.
 * @throws std::bad_alloc if callback, metadata, owner, or registry storage
 *         cannot allocate.
 * @throws std::invalid_argument if the source-authored candidate violates an
 *         internal registry invariant.
 * @note This source-private provider is compiled into the configured Host
 *       product only on the real Apple Metal profile. It exports no operation
 *       DSO symbol, public ABI record, native handle, or asynchronous owner.
 */
void register_metal_perlin_operation_provider();

}  // namespace ops
}  // namespace ps
