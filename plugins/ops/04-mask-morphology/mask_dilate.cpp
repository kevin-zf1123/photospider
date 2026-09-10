#include <algorithm>
#include <string>
#include <utility>

#include "05-filter/kernel_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace basic_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_mask_dilate(OperationRegistry* registry) {
  OperationDefinition definition;
  definition.key = "mask.dilate";
  auto& t = definition.traits;
  t.input_count = 1;
  t.input_schema.resize(1);
  for (auto& port : t.input_schema) {
    port.element_type_mask = 12;
    port.rank = 2;
  }
  t.requires_dense_output = true;
  t.shape_rule = OperationShapeRule::PreserveFirstInput;
  t.region_rule = OperationRegionRule::Whole;
  t.output_dtype_rule = OperationDtypeRule::Input;
  for (auto& port : t.input_schema)
    port = {OperationPortKind::Float32Mask, 0, 0};
  t.output_schema = t.input_schema[0];
  t.output_semantic_rule = OperationSemanticRule::PreserveInput;
  t.output_element_type = ElementType::Float32;
  t.parameter_schema = {natural("radius", 0, 64), string("footprint")};
  std::sort(t.parameter_schema.begin(), t.parameter_schema.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  definition.callback = [traits = t](const OperationInvocation& invocation) {
    return execute(
        [](auto sample_type, const OperationInvocation& call,
           MutableValue* output) {
          using T = decltype(sample_type);
          kernel_filter<T>(Kind::Dilate, call, output);
        },
        traits, invocation);
  };
  return registry->register_operation(std::move(definition));
}
}  // namespace ps::plugin_internal
