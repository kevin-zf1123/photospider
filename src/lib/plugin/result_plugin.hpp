#pragma once
#include <memory>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"
#include "photospider/plugin/result_operation_plugin_api.h"
namespace ps::plugin_internal {
struct ImportedResultPlugin {
  std::vector<OperationDefinition> operations;
  std::vector<ValidationDefinition> validators;
  std::vector<DiagnosticDefinition> diagnostics;
};
Result<ImportedResultPlugin> import_result_plugin(
    const ps_result_operation_plugin_api_v5*, std::shared_ptr<void> library);
}  // namespace ps::plugin_internal
