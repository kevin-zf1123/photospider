#include <vector>

#include "00-foundation/core_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace core_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_core_identity(OperationRegistry* registry) {
  return registry->register_operation(OperationDefinition{
      "core.identity",
      preserving(OperationTraits{1U,
                                 true,
                                 true,
                                 true,
                                 simulated_gpu,
                                 simulated_gpu,
                                 0U,
                                 8U,
                                 true,
                                 ElementType::Float64,
                                 OperationShapeRule::PreserveFirstInput,
                                 OperationRegionRule::Elementwise,
                                 0U,
                                 {},
                                 {},
                                 std::vector<OperationPortConstraint>(1),
                                 {/* output Value */}}),
      [](const OperationInvocation& invocation) -> Result<Value> {
        return Result<Value>(invocation.inputs.front());
      }});
}
}  // namespace ps::plugin_internal
