#include <algorithm>
#include <utility>

#include "10-analysis/histogram_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace basic_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_analysis_histogram_out_of_range(OperationRegistry* registry) {
  constexpr Kind kind = Kind::Outside;
  OperationDefinition definition;
  definition.key = "analysis.histogram_out_of_range";
  auto& t = definition.traits;
  t.input_count = 1;
  t.input_schema.resize(1);
  for (auto& port : t.input_schema) {
    port.element_type_mask = 12;
    port.rank = 2;
  }
  t.requires_dense_output = true;
  t.shape_rule = OperationShapeRule::Axes;
  t.region_rule = OperationRegionRule::Whole;
  t.output_dtype_rule = OperationDtypeRule::Declared;
  t.parameter_schema = {real("range_min"), real("range_max")};
  t.output_element_type = ElementType::Int64;
  t.output_axes = {{OperationExtentSource::Constant, 2, "", 0, 0, 0}};
  std::sort(t.parameter_schema.begin(), t.parameter_schema.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  definition.callback = [traits = t](const OperationInvocation& invocation) {
    return execute(
        [](auto sample_type, const OperationInvocation& call,
           MutableValue* output) {
          using T = decltype(sample_type);
          histogram<T>(kind, call, output);
        },
        traits, invocation);
  };
  return registry->register_operation(std::move(definition));
}
}  // namespace ps::plugin_internal
