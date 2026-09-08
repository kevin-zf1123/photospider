#pragma once
#include "photospider/plugin/operation_registry.hpp"
namespace ps::plugin_internal {
/** @brief Registers the two maintained CPU image operations before freeze. */
Status register_image_operations(OperationRegistry* registry);
}  // namespace ps::plugin_internal
