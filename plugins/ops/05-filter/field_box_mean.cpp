#include <algorithm>
#include <utility>

#include "05-filter/field_smoothing.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace basic_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_field_box_mean(OperationRegistry* registry) {
  OperationDefinition definition;
  definition.key = "field.box_mean";
  auto& t = definition.traits;
  t.input_count = 1;
  t.input_schema.resize(1);
  for (auto& port : t.input_schema) {
    port.element_type_mask = 12;
    port.rank = 2;
  }
  t.outputs[0].requires_dense_output = true;
  t.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  t.outputs[0].region_rule = OperationRegionRule::Halo;
  t.outputs[0].output_dtype_rule = OperationDtypeRule::Input;
  t.parameter_schema = {natural("radius", 1, 64)};
  t.outputs[0].halo_radius_parameter = "radius";
  t.workspace_bytes = 129 * 8;
  t.workspace_input_multiplier = 2;
  t.outputs[0].output_semantic_rule = OperationSemanticRule::PreserveInput;
  std::sort(t.parameter_schema.begin(), t.parameter_schema.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  definition.callback = [traits = t](const OperationInvocation& invocation) {
    return execute(
        [](auto sample_type, const OperationInvocation& call,
           MutableValue* output) {
          using T = decltype(sample_type);
          smooth<T>(Kind::Box, call, output);
        },
        traits, invocation);
  };
  return registry->register_operation(std::move(definition));
}
}  // namespace ps::plugin_internal
