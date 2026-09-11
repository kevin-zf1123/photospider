#include <utility>

#include "02-format-color/color_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace color_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_channel_merge(OperationRegistry* registry) {
  OperationDefinition operation;
  operation.key = "channel.merge";
  auto& t = operation.traits;
  t.outputs[0].output_semantic_rule =
      OperationSemanticRule::MergeChannelsParameter;
  t.input_count = 1;
  t.input_schema.resize(1);
  auto& port = t.input_schema[0];
  port.kind = OperationPortKind::Typed;
  port.rank = 3;
  port.element_type_mask = 12;
  t.outputs[0].output_schema.kind = OperationPortKind::Typed;
  t.outputs[0].output_dtype_rule = OperationDtypeRule::Input;
  t.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  t.outputs[0].requires_dense_output = true;

  t.outputs[0].shape_rule = OperationShapeRule::Axes;
  t.outputs[0].output_axes = {
      {OperationExtentSource::InputAxis, 1, {}, 0, 0, 0},
      {OperationExtentSource::InputAxis, 1, {}, 0, 1, 0}};

  port.kind = OperationPortKind::Value;
  port.rank = 2;
  t.input_count = 0;
  t.repeated_minimum = 2;
  t.repeated_maximum = 4;
  t.outputs[0].output_semantic_parameter = "semantic";
  t.parameter_schema = {{"semantic", OperationParameterType::String, true}};
  t.outputs[0].output_axes.push_back(
      {OperationExtentSource::InputCount, 1, {}, 0, 0, 0});

  operation.callback = [traits = t,
                        channel = true](const OperationInvocation& call) {
    return channel ? channels(call, traits) : colors(call, traits);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
