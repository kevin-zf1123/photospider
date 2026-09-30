#pragma once
#include <memory>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"
#include "photospider/plugin/result_operation_plugin_api.h"
namespace ps::plugin_internal {
Result<std::vector<OperationDefinition>> import_result_plugin(
    const ps_result_operation_plugin_api_v1*, std::shared_ptr<void> library);
}
