#include <algorithm>
#include <utility>

#include "00-foundation/basic_execution.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace basic_ops;  // NOLINT(build/namespaces)

template <class T>
void calculate(const OperationInvocation& call, MutableValue* output) {
  field(call.inputs[0]);
  const double low = parameter(call, "edge0"), high = parameter(call, "edge1");
  require(low < high, ErrorCode::InvalidArgument,
          "smoothstep interval must increase");
  each(call.output_region, call, [&](auto i, const auto& c) {
    const double x = read<T>(call.inputs[0], c);
    const double t = x <= low ? 0 : x >= high ? 1 : fraction(x, low, high);
    store<float>(output->data(), i, t * t * (3 - 2 * t));
  });
}
}  // namespace
Status register_field_smoothstep(OperationRegistry* registry) {
  OperationDefinition definition;
  definition.key = "field.smoothstep";
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
  t.outputs[0].output_dtype_rule = OperationDtypeRule::Declared;
  t.parameter_schema = {real("edge0"), real("edge1")};
  t.outputs[0].output_element_type = ElementType::Float32;
  t.outputs[0].output_semantic_rule = OperationSemanticRule::Establish;
  t.outputs[0].output_facets = {
      encode_semantic(coverage_semantics()).take_value()};
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
