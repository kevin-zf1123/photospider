#include <utility>

#include "00-foundation/tensor_program.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_core_constant(OperationRegistry* registry) {
  OperationDefinition operation;
  operation.key = "core.constant";
  operation.traits = tensor_ops::traits(0, sizeof(tensor_ops::Constant), 1);
  operation.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  operation.traits.parameter_schema = {
      {"value", OperationParameterType::Float64, true}};
  operation.start_result = [](const ResultProgramQuery&,
                              const BufferAllocator& allocator) {
    return ResultContinuation::make<tensor_ops::Constant>(allocator);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
