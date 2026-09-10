#include <algorithm>
#include <string>
#include <utility>

#include "00-foundation/basic_execution.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace basic_ops;  // NOLINT(build/namespaces)
template <class T>
void lut(const OperationInvocation& call, MutableValue* output) {
  const auto& source = call.inputs[0];
  const auto& table = call.inputs[1];
  field(source);
  generic(table);
  same_type(source, table);
  const auto n = table.descriptor().shape[0];
  require(n >= 2 && n <= (std::uint64_t{1} << 53), ErrorCode::TypeMismatch,
          "LUT requires representable N>=2");
  const auto a = parameter(call, "domain_min"),
             b = parameter(call, "domain_max");
  require(a < b, ErrorCode::InvalidArgument, "LUT domain must increase");
  const auto policy = text(call, "out_of_domain");
  choice(policy, "reject", "clip");
  each(table.region(), call, [&](auto, const auto& c) { read<T>(table, c); });
  each(call.output_region, call, [&](auto i, const auto& c) {
    const double q = read<T>(source, c);
    require(policy == "clip" || (q >= a && q <= b), ErrorCode::OperationFailed,
            "field query outside LUT domain");
    double result;
    if (q <= a) {
      result = read<T>(table, {0});
    } else if (q >= b) {
      result = read<T>(table, {n - 1});
    } else {
      const double position = fraction(q, a, b) * (n - 1);
      auto lower = std::min(static_cast<std::uint64_t>(position), n - 2);
      auto knot = [&](std::uint64_t j) {
        return interpolate(static_cast<double>(j), 0,
                           static_cast<double>(n - 1), a, b);
      };
      if (q < knot(lower) && lower > 0)
        --lower;
      else if (q > knot(lower + 1) && lower + 2 < n)
        ++lower;
      const double left = knot(lower), right = knot(lower + 1);
      require(left < right && q >= left && q <= right,
              ErrorCode::OperationFailed, "LUT coordinate cannot be resolved");
      result = interpolate(q, left, right, read<T>(table, {lower}),
                           read<T>(table, {lower + 1}));
    }
    store<T>(output->data(), i, result);
  });
}
}  // namespace
Status register_field_apply_lut_1d(OperationRegistry* registry) {
  OperationDefinition definition;
  definition.key = "field.apply_lut_1d";
  auto& t = definition.traits;
  t.input_count = 2;
  t.input_schema.resize(2);
  for (auto& port : t.input_schema) {
    port.element_type_mask = 12;
    port.rank = 2;
  }
  t.requires_dense_output = true;
  t.shape_rule = OperationShapeRule::PreserveFirstInput;
  t.region_rule = OperationRegionRule::Whole;
  t.output_dtype_rule = OperationDtypeRule::Input;
  t.parameter_schema = {real("domain_min"), real("domain_max"),
                        string("out_of_domain")};
  t.input_schema[1].rank = 1;
  std::sort(t.parameter_schema.begin(), t.parameter_schema.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  definition.callback = [traits = t](const OperationInvocation& invocation) {
    return execute(
        [](auto sample_type, const OperationInvocation& call,
           MutableValue* output) {
          using T = decltype(sample_type);
          lut<T>(call, output);
        },
        traits, invocation);
  };
  return registry->register_operation(std::move(definition));
}
}  // namespace ps::plugin_internal
