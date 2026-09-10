#include <utility>

#include "02-format-color/color_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace color_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_color_xyz_to_lab(OperationRegistry* registry) {
  constexpr std::size_t i = 7;
  OperationDefinition operation;
  operation.key = "color.xyz_to_lab";
  auto& t = operation.traits;
  t.output_semantic_rule = OperationSemanticRule::XyzToLab;
  t.input_count = 1;
  t.input_schema.resize(1);
  auto& port = t.input_schema[0];
  port.kind = OperationPortKind::Typed;
  port.rank = 3;
  port.element_type_mask = 12;
  t.output_schema.kind = OperationPortKind::Typed;
  t.output_dtype_rule = OperationDtypeRule::Input;
  t.shape_rule = OperationShapeRule::PreserveFirstInput;
  t.requires_dense_output = true;

  port.element_type_mask = 0;
  port.element_type = static_cast<std::uint32_t>(ElementType::Float32);
  port.semantic_kind = static_cast<std::uint32_t>(SemanticKind::Image);
  t.output_schema.semantic_kind =
      static_cast<std::uint32_t>(SemanticKind::Image);

  operation.callback = [traits = t,
                        channel = i < 3](const OperationInvocation& call) {
    return channel ? channels(call, traits) : colors(call, traits);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
