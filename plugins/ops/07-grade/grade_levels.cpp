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
  const double low = parameter(call, "black"), high = parameter(call, "white");
  const double out0 = parameter(call, "out_min"),
               out1 = parameter(call, "out_max");
  const double gamma = parameter(call, "gamma");
  require(low < high && out0 <= out1 && gamma > 0, ErrorCode::InvalidArgument,
          "invalid levels input/output interval or gamma");
  each(call.output_region, call, [&](auto i, const auto& c) {
    const double x = read<T>(call.inputs[0], c);
    if (gamma == 1) {
      const double bounded = std::clamp(x, low, high);
      store<T>(output->data(), i, interpolate(bounded, low, high, out0, out1));
      return;
    }
    const double t = x <= low ? 0 : x >= high ? 1 : fraction(x, low, high);
    const double powered =
        t == 0 || t == 1 ? t : finite(std::pow(t, finite(1 / gamma)));
    store<T>(output->data(), i, blend(out0, out1, powered));
  });
}
}  // namespace
Status register_grade_levels(OperationRegistry* registry) {
  OperationDefinition definition;
  definition.key = "grade.levels";
  auto& t = definition.traits;
  t.input_count = 1;
  t.input_schema.resize(1);
  for (auto& port : t.input_schema) {
    port.element_type_mask = 12;
    port.rank = 2;
  }
  t.requires_dense_output = true;
  t.shape_rule = OperationShapeRule::PreserveFirstInput;
  t.region_rule = OperationRegionRule::Elementwise;
  t.output_dtype_rule = OperationDtypeRule::Input;
  t.parameter_schema = {real("black"), real("white"), real("gamma", 0),
                        real("out_min"), real("out_max")};
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
