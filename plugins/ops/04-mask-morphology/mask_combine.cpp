#include <algorithm>
#include <string>
#include <utility>

#include "00-foundation/basic_execution.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace basic_ops;  // NOLINT(build/namespaces)

template <class T>
void calculate(const OperationInvocation& call, MutableValue* output) {
  const auto algebra = text(call, "algebra"),
             operation = text(call, "operation");
  choice(algebra, "fuzzy", "independent_coverage");
  require(operation == "and" || operation == "or" || operation == "xor",
          ErrorCode::InvalidArgument, "unknown mask operation");
  each(call.output_region, call, [&](auto i, const auto& c) {
    const double a = read<T>(call.inputs[0], c), b = read<T>(call.inputs[1], c);
    double result;
    if (algebra == "fuzzy")
      result = operation == "and"  ? std::min(a, b)
               : operation == "or" ? std::max(a, b)
                                   : std::abs(a - b);
    else
      result = operation == "and"  ? a * b
               : operation == "or" ? a + b - a * b
                                   : a + b - 2 * a * b;
    store<T>(output->data(), i, result);
  });
}
}  // namespace
Status register_mask_combine(OperationRegistry* registry) {
  OperationDefinition definition;
  definition.key = "mask.combine";
  auto& t = definition.traits;
  t.input_count = 2;
  t.input_schema.resize(2);
  for (auto& port : t.input_schema) {
    port.element_type_mask = 12;
    port.rank = 2;
  }
  t.outputs[0].requires_dense_output = true;
  t.outputs[0].shape_rule = OperationShapeRule::MatchAllInputs;
  t.outputs[0].region_rule = OperationRegionRule::Elementwise;
  t.outputs[0].output_dtype_rule = OperationDtypeRule::Input;
  for (auto& port : t.input_schema)
    port = {OperationPortKind::Float32Mask, 0, 0};
  t.outputs[0].output_schema = t.input_schema[0];
  t.outputs[0].output_semantic_rule = OperationSemanticRule::PreserveInput;
  t.outputs[0].output_element_type = ElementType::Float32;
  t.parameter_schema = {string("operation"), string("algebra")};
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
