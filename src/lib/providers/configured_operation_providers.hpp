#pragma once

namespace ps::providers {

/**
 * @brief Publishes core operations and every build-configured provider.
 *
 * @return Nothing.
 * @throws std::bad_alloc if callback or registry storage allocation fails.
 * @throws GraphError when a configured provider rejects initialization.
 * @throws std::system_error when provider one-time initialization cannot
 *         synchronize.
 * @note The process plugin manager invokes this while holding its state and
 *       registry ownership locks. Provider callbacks are replaceable through
 *       the ordinary operation-plugin registrar and are restored on unload.
 */
void register_configured_operation_providers();

}  // namespace ps::providers
