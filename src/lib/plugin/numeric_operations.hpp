#pragma once

#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal {
/** @brief Registers CPU Whole numeric foundations before registry freeze. */
Status register_numeric_operations(OperationRegistry* registry);
}  // namespace ps::plugin_internal
