#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "support/multi_output_result_fixture.hpp"

namespace operation_result {
inline ps::SchemaTemplate schema(bool faceted = false) {
  auto result = multi_result::schema();
  result.id = "fixture.scalar";
  if (faceted)
    result.tensors[0].facets = {ps::ValueFacet{"test.semantic", 2, {8, 9}}};
  return result;
}
inline ps::WorkflowDocument document(
    const std::string& key,
    const std::map<std::string, ps::ParameterValue>& parameters = {},
    bool faceted = false) {
  ps::WorkflowDocument result;
  result.inputs = {multi_result::declaration(1, "input", schema(faceted))};
  result.nodes = {{2, key, {ps::WorkflowInputReference{1}}, parameters}};
  result.outputs = {{"value", 2, "value"}};
  return result;
}
inline ps::Result<ps::ExecutionResult> execute(
    ps::OperationRegistry& registry, const std::string& key,
    const std::map<std::string, ps::ParameterValue>& parameters = {},
    bool faceted = false) {
  auto borrowed = std::shared_ptr<ps::OperationRegistry>(
      &registry, [](ps::OperationRegistry*) {});
  ps::GraphContext graph(document(key, parameters, faceted));
  auto compiled = ps::Compiler(borrowed).compile(graph);
  if (!compiled.ok())
    return ps::Result<ps::ExecutionResult>(compiled.status());
  ps::ExecutionContextConfig config;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(borrowed, config);
  auto input = multi_result::binding(context.resource_budget().take_value(),
                                     "input", 3, schema(faceted));
  return context.execute(compiled.value().plan, {{input}});
}
inline ps::Result<ps::ResultContinuation> start(
    ps::OperationRegistry& registry, const std::string& key,
    const std::map<std::string, ps::ParameterValue>& parameters,
    ps::Backend backend, bool invalid_input = false) {
  ps::ResultProgramMetadata metadata;
  metadata.inputs.resize(1);
  if (!invalid_input)
    metadata.inputs[0].result_schema =
        std::make_shared<const ps::SchemaTemplate>(schema(true));
  metadata.output.result_schema =
      std::make_shared<const ps::SchemaTemplate>(schema(true));
  ps::ResultProgramQuery query(metadata, parameters);
  query.backend = backend;
  query.semantic_key = key;
  ps::ResourceBudget root;
  return registry.start_result(key, query, root.allocator());
}
}  // namespace operation_result
