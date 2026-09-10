#include <utility>

#include "02-format-color/color_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace color_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_channel_swizzle(OperationRegistry* registry) {
  OperationDefinition operation;
  operation.key = "channel.swizzle";
  auto& t = operation.traits;
  t.output_semantic_rule = OperationSemanticRule::SwizzleChannels;
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

  t.shape_rule = OperationShapeRule::Axes;
  t.output_axes = {{OperationExtentSource::InputAxis, 1, {}, 0, 0, 0},
                   {OperationExtentSource::InputAxis, 1, {}, 0, 1, 0}};

  port.kind = OperationPortKind::Value;
  t.output_schema.kind = OperationPortKind::Value;
  t.output_semantic_parameter = "indices";
  t.parameter_schema = {{"indices", OperationParameterType::String, true}};
  t.output_axes.push_back(
      {OperationExtentSource::IndexListCount, 1, "indices", 0, 0, 0});

  operation.callback = [traits = t,
                        channel = true](const OperationInvocation& call) {
    return channel ? channels(call, traits) : colors(call, traits);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
