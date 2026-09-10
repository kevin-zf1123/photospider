#include <utility>

#include "02-format-color/color_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace color_ops;  // NOLINT(build/namespaces)

}  // namespace
Status register_channel_extract(OperationRegistry* registry) {
  OperationDefinition operation;
  operation.key = "channel.extract";
  auto& t = operation.traits;
  t.output_semantic_rule = OperationSemanticRule::ExtractChannel;
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

  t.output_semantic_parameter = "index";
  t.parameter_schema = {
      {"index", OperationParameterType::Int64, true, true, 0, 63}};

  operation.callback = [traits = t,
                        channel = true](const OperationInvocation& call) {
    return channel ? channels(call, traits) : colors(call, traits);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
