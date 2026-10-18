#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/kernel_registry_fixture.hpp"
#include "support/result_image_fixture.hpp"

namespace ps::test_image {
struct Driver {
  std::shared_ptr<OperationRegistry> registry = ps::test::kernel_registry();
  ExecutionContextConfig config;
  std::unique_ptr<ExecutionContext> context;
  ResourceBudget root;
  bool native = false;
  explicit Driver(bool gpu = false) : native(gpu) {
    config.gpu_enabled = gpu;
    config.managed_resources = ResourceLimits{};
    context = std::make_unique<ExecutionContext>(registry, config);
    root = take(context->resource_budget());
  }
  struct Run {
    std::shared_ptr<GraphContext> graph;
    ExecutionPlan plan;
    ExecutionBindings bindings;
  };
  Run prepare(const std::string& operation,
              std::vector<ExecutionBinding> inputs,
              std::map<std::string, ParameterValue> parameters = {},
              const std::string& output = "value",
              PlanningOptions planning = {}) {
    WorkflowDocument document;
    WorkflowNode node;
    node.id = 1;
    node.operation = operation;
    node.parameters = std::move(parameters);
    for (unsigned i = 0; i < inputs.size(); ++i) {
      WorkflowInputDeclaration declaration;
      declaration.id = i + 1;
      declaration.name = inputs[i].name;
      if (inputs[i].result.valid()) {
        declaration.result_schema =
            std::make_shared<SchemaTemplate>(inputs[i].result.schema());
      }
      document.inputs.push_back(std::move(declaration));
      node.inputs.push_back(WorkflowInputReference{i + 1});
    }
    document.nodes.push_back(std::move(node));
    document.outputs = {{"out", 1, output}};
    auto graph = std::make_shared<GraphContext>(document);
    planning.execution_mode =
        native ? ExecutionMode::NativeGpu : ExecutionMode::CpuExact;
    auto compiled = take(Compiler(registry).compile(*graph, planning));
    return {std::move(graph), std::move(compiled.plan), {std::move(inputs)}};
  }
  ResultRef run(const std::string& operation,
                std::vector<ExecutionBinding> inputs,
                std::map<std::string, ParameterValue> parameters = {},
                const std::string& output = "value") {
    auto prepared =
        prepare(operation, std::move(inputs), std::move(parameters), output);
    return take(context->execute(prepared.plan, prepared.bindings))
        .results.at("out");
  }
};
}  // namespace ps::test_image
