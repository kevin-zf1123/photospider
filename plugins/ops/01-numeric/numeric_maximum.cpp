#include <algorithm>
#include <utility>

#include "00-foundation/basic_execution.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace basic_ops;  // NOLINT(build/namespaces)

template <class T>
void calculate(const OperationInvocation& call, MutableValue* output) {
  same_type(call.inputs[0], call.inputs[1]);
  each(call.output_region, call, [&](auto i, const auto& c) {
    const double a = read<T>(call.inputs[0], c), b = read<T>(call.inputs[1], c);
    const double result = a == 0 && b == 0 ? 0.0 : std::max(a, b);
    store<T>(output->data(), i, result);
  });
}
}  // namespace
Status register_numeric_maximum(OperationRegistry* registry) {
  OperationDefinition definition;
  definition.key = "numeric.maximum";
  auto& t = definition.traits;
  t.input_count = 2;
  t.input_schema.resize(2);
  for (auto& port : t.input_schema) {
    port.element_type_mask = 12;
    port.rank = 0;
  }
  t.requires_dense_output = true;
  t.shape_rule = OperationShapeRule::MatchAllInputs;
  t.region_rule = OperationRegionRule::Elementwise;
  t.output_dtype_rule = OperationDtypeRule::Input;
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
