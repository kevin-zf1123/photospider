#include <vector>

#include "00-foundation/core_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace core_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_core_gpu_fallback_probe(OperationRegistry* registry) {
  return registry->register_operation(OperationDefinition{
      "core.gpu_fallback_probe", preserving([&] {
        OperationTraits t;
        t.input_count = 1U;
        t.deterministic = true;
        t.side_effect_free = true;
        t.supports_cpu = true;
        t.supports_gpu = true;
        t.allows_cpu_fallback = true;
        t.estimated_bytes = 0U;
        t.version = 9U;
        t.cacheable = true;
        t.outputs[0].output_element_type = ElementType::Float64;
        t.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
        t.outputs[0].region_rule = OperationRegionRule::Elementwise;
        t.outputs[0].halo_radius = 0U;
        t.parameter_schema = {};
        t.outputs[0].fixed_output_shape = {};
        t.input_schema = std::vector<OperationPortConstraint>(1);
        t.outputs[0].output_schema = {/* output Value */};
        return t;
      }()),
      [](const OperationInvocation& invocation) -> Result<Value> {
        if (invocation.backend == Backend::Gpu) {
          return Result<Value>(Status::failure(ErrorCode::BackendUnavailable,
                                               "probe rejects GPU execution"));
        }
        return Result<Value>(invocation.inputs.front());
      }});
}
}  // namespace ps::plugin_internal
