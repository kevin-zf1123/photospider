#include <utility>
#include <vector>

#include "04-mask-morphology/component_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace component_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_component_area(OperationRegistry* registry) {
  OperationDefinition op;
  op.key = "component.area";
  auto& t = op.traits;
  t.input_count = 1;
  t.input_schema.resize(1);
  auto& port = t.input_schema[0];
  port.kind = OperationPortKind::Typed;
  port.rank = 2;
  port.element_type = static_cast<std::uint32_t>(ElementType::Int64);
  t.outputs[0].output_element_type = ElementType::Int64;
  t.outputs[0].shape_rule = OperationShapeRule::Scalar;
  t.outputs[0].requires_dense_output = true;

  port.facets =
      false ? std::vector<ValueFacet>{encode_semantic(coverage_semantics())
                                          .take_value()}
            : label_facets();
  t.parameter_schema = {{"capacity", OperationParameterType::Int64, true, true,
                         1, 0x1fffffffffffffp0}};

  t.outputs[0].shape_rule = OperationShapeRule::Axes;
  t.outputs[0].output_axes = {
      {OperationExtentSource::Parameter, 1, "capacity", 0, 0, 1}};

  op.callback = [attribute = 3 - 2](const OperationInvocation& call) {
    return attributes(call, attribute);
  };

  return registry->register_operation(std::move(op));
}
}  // namespace ps::plugin_internal
