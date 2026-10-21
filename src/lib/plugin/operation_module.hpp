#pragma once

#include <memory>
#include <vector>

#include "photospider/plugin/operation_module_api.h"
#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal {
Result<std::vector<OperationDefinition>> import_operation_module(
    const ps_operation_module_v8* api, std::shared_ptr<void> library);
}  // namespace ps::plugin_internal
