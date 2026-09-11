#include <utility>

#include "05-filter/regional_convolution.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_field_convolve(OperationRegistry* registry) {
  OperationDefinition op;
  op.key = "field.convolve";
  auto& traits = op.traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  for (auto& input : traits.input_schema) {
    input.element_type_mask = 12;
    input.rank = 2;
  }
  auto& output = traits.outputs[0];
  output.shape_rule = OperationShapeRule::PreserveFirstInput;
  output.output_dtype_rule = OperationDtypeRule::Input;
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(convolution::State);
  output.maximum_dependency_stages = 2;
  traits.parameter_schema = convolution::parameters(false);
  op.validate_dependency = [](const auto& inputs, const auto& parameters) {
    return convolution::validate(inputs, parameters, false);
  };
  op.start_dependency = [](const DependencyQuery&,
                           const BufferAllocator& allocator) {
    return DependencyContinuation::make<convolution::State>(allocator, false,
                                                            0);
  };
  return registry->register_operation(std::move(op));
}
}  // namespace ps::plugin_internal
