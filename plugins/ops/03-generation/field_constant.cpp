#include <algorithm>
#include <string>
#include <utility>

#include "03-generation/field_generation.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace basic_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_field_constant(OperationRegistry* registry) {
  OperationDefinition definition;
  definition.key = "field.constant";
  auto& t = definition.traits;
  t.input_count = 0;
  t.input_schema.resize(0);
  t.requires_dense_output = true;
  t.shape_rule = OperationShapeRule::Axes;
  t.region_rule = OperationRegionRule::Whole;
  t.output_dtype_rule = OperationDtypeRule::Parameter;
  t.parameter_schema = {natural("height", 1, 0x1fffffffffffff),
                        natural("width", 1, 0x1fffffffffffff), string("dtype")};
  t.output_dtype_parameter = "dtype";
  t.output_axes = {{OperationExtentSource::Parameter, 1, "height", 0, 0, 0},
                   {OperationExtentSource::Parameter, 1, "width", 0, 0, 0}};
  t.parameter_schema.push_back(real("value"));
  std::sort(t.parameter_schema.begin(), t.parameter_schema.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  definition.callback = [traits = t](const OperationInvocation& invocation) {
    return execute(
        [](auto sample_type, const OperationInvocation& call,
           MutableValue* output) {
          using T = decltype(sample_type);
          generator<T>(Kind::Constant, call, output);
        },
        traits, invocation);
  };
  return registry->register_operation(std::move(definition));
}
}  // namespace ps::plugin_internal
