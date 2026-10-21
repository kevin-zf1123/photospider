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
  std::shared_ptr<PlannerRegistry> planners =
      std::make_shared<PlannerRegistry>();
  Driver() {
    const auto defaults = make_default_planner_registry();
    const auto definition = *defaults->find("static-tiles");
    require(planners->add(definition).ok(), "default planner registration");
    require(planners
                ->add({"broadcast-inputs", "1", "Explicit zero-stride inputs",
                       [lower = definition.plan](
                           const ValidatedIR& ir, PlanBuilder& builder,
                           const PlanningOptions& options) {
                         auto status = lower(ir, builder, options);
                         if (!status.ok())
                           return status;
                         auto& program = builder.program();
                         for (auto input : program.inputs)
                           for (auto ti : program.values[input].tiles) {
                             auto& tile = program.tiles[ti];
                             auto layout = program.layouts[tile.layout];
                             layout.strides.fill(0);
                             layout.byte_offset = 0;
                             tile.layout = static_cast<physical::Id>(
                                 program.layouts.size());
                             program.layouts.push_back(layout);
                             tile.bytes =
                                 TensorSpan::element_size(layout.element_type);
                             for (auto& task : program.tasks)
                               for (auto& read : task.reads)
                                 if (read.tile == ti)
                                   read.layout = tile.layout;
                           }
                         return physical::finalize_program(&program);
                       }})
                .ok(),
            "broadcast planner registration");
    planners->freeze();
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
    auto compiled =
        take(Compiler(registry, {}, planners).compile(*graph, planning));
    return {std::move(graph),
            std::move(compiled.plan.value()),
            {std::move(inputs)}};
  }
  ResultRef run(const std::string& operation,
                std::vector<ExecutionBinding> inputs,
                std::map<std::string, ParameterValue> parameters = {},
                const std::string& output = "value") {
    auto prepared =
        prepare(operation, std::move(inputs), std::move(parameters), output);
    return take(multi_result::run(*context, prepared.plan, prepared.bindings))
        .results.at("out");
  }
};
}  // namespace ps::test_image
