#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "support/multi_output_result_fixture.hpp"

namespace operation_result {
inline ps::SchemaTemplate schema(bool atomic = false) {
  auto result = multi_result::schema();
  result.id = "fixture.scalar";
  if (atomic)
    result.tensors[0].atomic_trailing_axes = 1;
  return result;
}
inline ps::WorkflowDocument document(
    const std::string& key,
    const std::map<std::string, ps::ParameterValue>& parameters = {},
    bool atomic = false) {
  ps::WorkflowDocument result;
  result.inputs = {multi_result::declaration(1, "input", schema(atomic))};
  result.nodes = {{2, key, {ps::WorkflowInputReference{1}}, parameters}};
  result.outputs = {{"value", 2, "value"}};
  return result;
}
inline ps::Result<ps::ExecutionResult> execute(
    ps::OperationRegistry& registry, const std::string& key,
    const std::map<std::string, ps::ParameterValue>& parameters = {},
    bool atomic = false) {
  auto borrowed = std::shared_ptr<ps::OperationRegistry>(
      &registry, [](ps::OperationRegistry*) {});
  ps::GraphContext graph(document(key, parameters, atomic));
  auto compiled = ps::Compiler(borrowed).compile(graph);
  if (!compiled.ok())
    return ps::Result<ps::ExecutionResult>(compiled.status());
  ps::ExecutionContextConfig config;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(borrowed, config);
  auto input = multi_result::binding(context.resource_budget().take_value(),
                                     "input", 3, schema(atomic));
  return multi_result::run(context, compiled.value().plan.value(), {{input}});
}
}  // namespace operation_result
