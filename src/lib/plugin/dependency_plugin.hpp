#pragma once
#include <memory>

#include "photospider/plugin/dependency_plugin_api.h"
#include "photospider/plugin/operation_registry.hpp"
namespace ps::plugin_internal {
/** @brief Copies a validated C program while retaining its DSO owner. */
Status prepare_dependency_plugin(OperationDefinition* definition,
                                 const ps_dependency_program_v8* program,
                                 void* user_data,
                                 std::shared_ptr<const void> library);
}  // namespace ps::plugin_internal
