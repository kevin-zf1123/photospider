#include <vector>

#include "00-foundation/core_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace core_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_core_delay(OperationRegistry* registry) {
  return registry->register_operation(OperationDefinition{
      "core.delay", preserving([&] {
        OperationTraits t;
        t.input_count = 1U;
        t.deterministic = true;
        t.side_effect_free = true;
        t.supports_cpu = true;
        t.supports_gpu = false;
        t.allows_cpu_fallback = false;
        t.estimated_bytes = 0U;
        t.version = 9U;
        t.cacheable = false;
        t.outputs[0].output_element_type = ElementType::Float64;
        t.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
        t.outputs[0].region_rule = OperationRegionRule::Whole;
        t.outputs[0].halo_radius = 0U;
        t.parameter_schema = {OperationParameterSpec{
            "milliseconds", OperationParameterType::Int64, true}};
        t.outputs[0].fixed_output_shape = {};
        t.input_schema = std::vector<OperationPortConstraint>(1);
        t.outputs[0].output_schema = {/* output Value */};
        return t;
      }()),
      [](const OperationInvocation& invocation) -> Result<Value> {
        auto milliseconds =
            integer_parameter(invocation.parameters, "milliseconds");
        if (!milliseconds.ok() || milliseconds.value() < 0 ||
            milliseconds.value() > 5000) {
          return Result<Value>(Status::failure(
              ErrorCode::InvalidArgument,
              "delay milliseconds must be an int64 in 0..5000"));
        }
        for (std::int64_t elapsed = 0; elapsed < milliseconds.value();
             ++elapsed) {
          if (invocation.cancellation.cancelled()) {
            return Result<Value>(
                Status::failure(ErrorCode::Cancelled, "delay was cancelled"));
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return Result<Value>(invocation.inputs.front());
      }});
}
}  // namespace ps::plugin_internal
