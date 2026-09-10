#include <vector>

#include "00-foundation/core_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace core_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_math_add(OperationRegistry* registry) {
  return registry->register_operation(OperationDefinition{
      "math.add",
      float64_inputs(OperationTraits{2U,
                                     true,
                                     true,
                                     true,
                                     simulated_gpu,
                                     simulated_gpu,
                                     sizeof(double),
                                     7U,
                                     true,
                                     ElementType::Float64,
                                     OperationShapeRule::MatchAllInputs,
                                     OperationRegionRule::Elementwise,
                                     0U,
                                     {},
                                     {},
                                     std::vector<OperationPortConstraint>(2),
                                     {/* output Value */}}),
      [](const OperationInvocation& invocation) -> Result<Value> {
        auto left = invocation.inputs[0].as_float64();
        auto right = invocation.inputs[1].as_float64();
        if (!left.ok()) {
          return Result<Value>(left.status());
        }
        if (!right.ok()) {
          return Result<Value>(right.status());
        }
        return Result<Value>(
            allocated_scalar(invocation, left.value() + right.value()));
      }});
}
}  // namespace ps::plugin_internal
