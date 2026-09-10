#include <utility>
#include <vector>

#include "04-mask-morphology/component_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace component_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_component_bbox(OperationRegistry* registry) {
  constexpr unsigned kind = 4;
  OperationDefinition op;
  op.key = "component.bbox";
  auto& t = op.traits;
  t.input_count = 1;
  t.input_schema.resize(1);
  auto& port = t.input_schema[0];
  port.kind = OperationPortKind::Typed;
  port.rank = 2;
  port.element_type = static_cast<std::uint32_t>(kind < 2 ? ElementType::Float32
                                                          : ElementType::Int64);
  t.output_element_type = kind == 0 ? ElementType::Float32 : ElementType::Int64;
  t.shape_rule = kind < 2 ? OperationShapeRule::PreserveFirstInput
                          : OperationShapeRule::Scalar;
  t.requires_dense_output = true;

  port.facets =
      kind == 1 ? std::vector<ValueFacet>{encode_semantic(coverage_semantics())
                                              .take_value()}
                : label_facets();
  t.parameter_schema = {{"capacity", OperationParameterType::Int64, true, true,
                         1, 0x1fffffffffffffp0}};

  t.shape_rule = OperationShapeRule::Axes;
  t.output_axes = {{OperationExtentSource::Parameter, 1, "capacity", 0, 0, 1}};
  t.output_axes.push_back({OperationExtentSource::Constant, 4, {}, 0, 0, 0});

  op.callback = [attribute = kind - 2](const OperationInvocation& call) {
    return attributes(call, attribute);
  };

  return registry->register_operation(std::move(op));
}
}  // namespace ps::plugin_internal
