#pragma once

namespace ps::providers {

/**
 * @brief Publishes core operations and every build-configured provider.
 *
 * Dependency-enabled products publish the OpenCV CPU provider. The real Apple
 * Metal profile additionally publishes the source-private Metal Perlin device
 * candidate without exposing it through operation ABI v1.
 *
 * @return Nothing.
 * @throws std::bad_alloc if callback or registry storage allocation fails.
 * @throws GraphError when a configured provider rejects initialization.
 * @throws std::system_error when provider one-time initialization cannot
 *         synchronize.
 * @throws std::invalid_argument when a source-authored configured candidate
 *         violates an internal registry invariant.
 * @note The process plugin manager invokes this while holding its state and
 *       registry ownership locks. Provider callbacks are replaceable through
 *       the ordinary operation-plugin registrar and are restored on unload.
 */
void register_configured_operation_providers();

}  // namespace ps::providers
