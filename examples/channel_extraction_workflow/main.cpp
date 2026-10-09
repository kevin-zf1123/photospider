#include <iostream>
#include <vector>

#include "photospider/ops/format/channel.hpp"
#include "source.hpp"  // NOLINT(build/include_subdir)

int main() try {
  using namespace channel_fixture;  // NOLINT(build/namespaces)
  ps::TensorDescription description;
  description.channel_axis = 2;
  description.channels = {{"B", "blue", "relative"},
                          {"A", "coverage", "ratio"},
                          {"R", "red", "relative"},
                          {"G", "green", "relative"}};
  auto input = source({ps::ElementType::UInt8, {2, 2, 4}},
                      {take(ps::encode_tensor_description(description))});
  input.bytes = {30, 25, 10, 20, 31, 50, 11, 21,
                 32, 75, 12, 22, 33, 99, 13, 23};
  auto registry = ps::make_default_operation_registry();
  ps::ExecutionContext execution(registry);
  auto value = publish(take(execution.resource_budget()), input);
  ps::WorkflowDocument document;
  document.inputs = {declaration(input)};
  ps::OperationMetadata metadata;
  metadata.result_schema = document.inputs[0].result_schema;
  auto handles = take(ps::format::split_channels(
      document, ps::WorkflowInputReference{1}, metadata));
  document.outputs = {{"red", handles[2].output.source_node, "values"}};
  ps::PlanningOptions options;
  const ps::Region roi({{1, 1}, {0, 2}});
  options.output_regions = {{"red", roi}};
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph, options));
  auto result = take(execution.execute(compiled.plan, {{{"source", value}}}));
  check(read(result.results.at("red"), roi) ==
            std::vector<std::uint8_t>({12, 13}),
        "red oracle");
  for (const auto& timing : result.diagnostics.operation_timings)
    check(timing.output.node_id == handles[2].output.source_node,
          "unrequested split sibling executed");
  std::cout
      << "Result split: red ROI = [12, 13]; only requested channel executed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
