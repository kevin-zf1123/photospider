#pragma once

namespace ps::providers::opencv {

/**
 * @brief Initializes and publishes the repository-owned OpenCV CPU operations.
 *
 * Registration first applies the provider-local process policy and then
 * publishes every OpenCV-backed monolithic, tiled, and ROI callback into the
 * process registry.
 *
 * @return Nothing.
 * @throws std::bad_alloc if callback or registry storage allocation fails, or
 *         when OpenCV reports resource exhaustion.
 * @throws GraphError with `GraphErrc::ComputeError` when OpenCV initialization
 *         fails for another reason.
 * @throws std::system_error if one-time initialization synchronization fails.
 * @note This private provider exists only when
 *       `PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER` is enabled. It owns
 *       OpenCV process initialization, algorithm calls, and exception
 *       translation. Registry locking covers publication, never execution;
 *       callbacks are reentrant unless their own documentation says otherwise.
 */
void register_provider();

}  // namespace ps::providers::opencv
