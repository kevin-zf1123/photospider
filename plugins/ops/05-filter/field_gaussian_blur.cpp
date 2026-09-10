#include <algorithm>
#include <utility>

#include "05-filter/field_smoothing.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace basic_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_field_gaussian_blur(OperationRegistry* registry) {
  constexpr Kind kind = Kind::Gaussian;
  OperationDefinition definition;
  definition.key = "field.gaussian_blur";
  auto& t = definition.traits;
  t.input_count = 1;
  t.input_schema.resize(1);
  for (auto& port : t.input_schema) {
    port.element_type_mask = 12;
    port.rank = 2;
  }
  t.requires_dense_output = true;
  t.shape_rule = OperationShapeRule::PreserveFirstInput;
  t.region_rule = OperationRegionRule::Halo;
  t.output_dtype_rule = OperationDtypeRule::Input;
  t.parameter_schema = {natural("radius", 1, 64)};
  t.parameter_schema.push_back(real("sigma", 0, 64));
  t.halo_radius_parameter = "radius";
  t.workspace_bytes = 129 * 8;
  t.workspace_input_multiplier = 2;
  t.output_semantic_rule = OperationSemanticRule::PreserveInput;
  std::sort(t.parameter_schema.begin(), t.parameter_schema.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  definition.callback = [traits = t](const OperationInvocation& invocation) {
    return execute(
        [](auto sample_type, const OperationInvocation& call,
           MutableValue* output) {
          using T = decltype(sample_type);
          smooth<T>(kind, call, output);
        },
        traits, invocation);
  };
  return registry->register_operation(std::move(definition));
}
}  // namespace ps::plugin_internal
