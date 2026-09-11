#include <algorithm>
#include <utility>

#include "00-foundation/basic_execution.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace basic_ops;  // NOLINT(build/namespaces)

template <class T>
void calculate(const OperationInvocation& call, MutableValue* output) {
  each(call.output_region, call, [&](auto i, const auto& c) {
    store<T>(output->data(), i, 1 - read<T>(call.inputs[0], c));
  });
}
}  // namespace
Status register_mask_invert(OperationRegistry* registry) {
  OperationDefinition definition;
  definition.key = "mask.invert";
  auto& t = definition.traits;
  t.input_count = 1;
  t.input_schema.resize(1);
  for (auto& port : t.input_schema) {
    port.element_type_mask = 12;
    port.rank = 2;
  }
  t.outputs[0].requires_dense_output = true;
  t.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  t.outputs[0].region_rule = OperationRegionRule::Elementwise;
  t.outputs[0].output_dtype_rule = OperationDtypeRule::Input;
  for (auto& port : t.input_schema)
    port = {OperationPortKind::Float32Mask, 0, 0};
  t.outputs[0].output_schema = t.input_schema[0];
  t.outputs[0].output_semantic_rule = OperationSemanticRule::PreserveInput;
  t.outputs[0].output_element_type = ElementType::Float32;
  std::sort(t.parameter_schema.begin(), t.parameter_schema.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  definition.callback = [traits = t](const OperationInvocation& invocation) {
    return execute(
        [](auto sample_type, const OperationInvocation& call,
           MutableValue* output) {
          using T = decltype(sample_type);
          calculate<T>(call, output);
        },
        traits, invocation);
  };
  return registry->register_operation(std::move(definition));
}
}  // namespace ps::plugin_internal
