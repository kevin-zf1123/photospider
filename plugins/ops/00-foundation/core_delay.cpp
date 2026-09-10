#include <vector>

#include "00-foundation/core_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace core_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_core_delay(OperationRegistry* registry) {
  return registry->register_operation(OperationDefinition{
      "core.delay",
      preserving(OperationTraits{
          1U,
          true,
          true,
          true,
          false,
          false,
          0U,
          8U,
          false,
          ElementType::Float64,
          OperationShapeRule::PreserveFirstInput,
          OperationRegionRule::Whole,
          0U,
          {OperationParameterSpec{"milliseconds", OperationParameterType::Int64,
                                  true}},
          {},
          std::vector<OperationPortConstraint>(1),
          {/* output Value */}}),
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
