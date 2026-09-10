#pragma once

#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal {
/** @brief Registers CPU Whole channel, alpha and color foundation operations.
 */
Status register_color_operations(OperationRegistry* registry);
}  // namespace ps::plugin_internal
