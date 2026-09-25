#pragma once
#include <memory>

#include "photospider/plugin/operation_registry.hpp"
#include "photospider/plugin/planar_operation_plugin_api.h"
namespace ps::plugin_internal {
Status prepare_planar_plugin(OperationDefinition*,
                             const ps_planar_operation_v1&, void*,
                             std::shared_ptr<void>);
}  // namespace ps::plugin_internal
