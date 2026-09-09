#pragma once

#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal {
/** @brief Registers bounded CPU Whole expression sampling and linear 1D LUT. */
Status register_expression_operations(OperationRegistry* registry);
}  // namespace ps::plugin_internal
