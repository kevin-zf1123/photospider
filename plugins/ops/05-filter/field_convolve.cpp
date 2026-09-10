#include <algorithm>
#include <string>
#include <utility>

#include "05-filter/kernel_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace basic_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_field_convolve(OperationRegistry* registry) {
  constexpr Kind kind = Kind::Convolve;
  OperationDefinition definition;
  definition.key = "field.convolve";
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
  t.parameter_schema = {natural("anchor_y", 0, 0x1fffffffffffff),
                        natural("anchor_x", 0, 0x1fffffffffffff),
                        string("boundary")};
  std::sort(t.parameter_schema.begin(), t.parameter_schema.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  definition.callback = [traits = t](const OperationInvocation& invocation) {
    return execute(
        [](auto sample_type, const OperationInvocation& call,
           MutableValue* output) {
          using T = decltype(sample_type);
          kernel_filter<T>(kind, call, output);
        },
        traits, invocation);
  };
  return registry->register_operation(std::move(definition));
}
}  // namespace ps::plugin_internal
