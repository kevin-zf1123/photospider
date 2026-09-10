#pragma once

#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal {
/** @brief Registers CPU Whole threshold, four-connected labels and attributes.
 */
Status register_component_operations(OperationRegistry* registry);
}  // namespace ps::plugin_internal
