#include <vector>

#include "00-foundation/core_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace core_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_core_constant(OperationRegistry* registry) {
  return registry->register_operation(OperationDefinition{
      "core.constant",
      OperationTraits{0U,
                      true,
                      true,
                      true,
                      simulated_gpu,
                      simulated_gpu,
                      sizeof(double),
                      8U,
                      true,
                      ElementType::Float64,
                      OperationShapeRule::Scalar,
                      OperationRegionRule::Whole,
                      0U,
                      {OperationParameterSpec{
                          "value", OperationParameterType::Float64, true}},
                      {},
                      std::vector<OperationPortConstraint>(0),
                      {/* output Value */}},
      [](const OperationInvocation& invocation) -> Result<Value> {
        auto value = floating_parameter(invocation.parameters, "value");
        if (!value.ok()) {
          return Result<Value>(value.status());
        }
        return Result<Value>(allocated_scalar(invocation, value.value()));
      }});
}
}  // namespace ps::plugin_internal
