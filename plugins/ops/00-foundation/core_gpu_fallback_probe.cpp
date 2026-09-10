#include <vector>

#include "00-foundation/core_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace core_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_core_gpu_fallback_probe(OperationRegistry* registry) {
  return registry->register_operation(OperationDefinition{
      "core.gpu_fallback_probe",
      preserving(OperationTraits{1U,
                                 true,
                                 true,
                                 true,
                                 true,
                                 true,
                                 0U,
                                 7U,
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
        if (invocation.backend == Backend::Gpu) {
          return Result<Value>(Status::failure(ErrorCode::BackendUnavailable,
                                               "probe rejects GPU execution"));
        }
        return Result<Value>(invocation.inputs.front());
      }});
}
}  // namespace ps::plugin_internal
