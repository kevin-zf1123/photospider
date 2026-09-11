#include <algorithm>
#include <string>
#include <utility>

#include "01-numeric/curve_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace basic_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_curve_sample_linear(OperationRegistry* registry) {
  OperationDefinition definition;
  definition.key = "curve.sample_linear";
  auto& t = definition.traits;
  t.input_count = 1;
  t.input_schema.resize(1);
  for (auto& port : t.input_schema) {
    port.element_type_mask = 12;
    port.rank = 2;
  }
  t.outputs[0].requires_dense_output = true;
  t.outputs[0].shape_rule = OperationShapeRule::Axes;
  t.outputs[0].region_rule = OperationRegionRule::Whole;
  t.outputs[0].output_dtype_rule = OperationDtypeRule::Input;
  t.parameter_schema = {real("domain_min"), real("domain_max"),
                        string("out_of_domain")};
  t.parameter_schema.push_back(natural("count", 2, 1048576));
  t.outputs[0].output_axes = {
      {OperationExtentSource::Parameter, 1, "count", 0, 0, 0}};
  std::sort(t.parameter_schema.begin(), t.parameter_schema.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  definition.callback = [traits = t](const OperationInvocation& invocation) {
    return execute(
        [](auto sample_type, const OperationInvocation& call,
           MutableValue* output) {
          using T = decltype(sample_type);
          curve<T>(Kind::Linear, call, output);
        },
        traits, invocation);
  };
  return registry->register_operation(std::move(definition));
}
}  // namespace ps::plugin_internal
