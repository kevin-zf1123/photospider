#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "channel_extraction_workflow/source.hpp"

namespace {
template <class T>
T checked(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
}  // namespace
int main() try {
  auto registry = ps::make_default_operation_registry();
  ps::ExecutionContext context(registry);
  const auto root = checked(context.resource_budget());
  ps::WorkflowDocument document;
  ps::ExecutionBindings bindings;
  std::vector<ps::format::ChannelEditInput> inputs;
  const auto add = [&](const std::string& name,
                       const std::vector<std::uint64_t>& shape,
                       const std::vector<float>& samples,
                       ps::format::ChannelEditStructure structure) {
    auto source = channel_fixture::source({ps::ElementType::Float32, shape});
    std::memcpy(source.bytes.data(), samples.data(), source.bytes.size());
    const auto id = document.inputs.size() + 1;
    auto declaration = channel_fixture::declaration(source);
    declaration.id = id;
    declaration.name = name;
    ps::OperationMetadata metadata;
    metadata.result_schema = declaration.result_schema;
    document.inputs.push_back(std::move(declaration));
    bindings.inputs.push_back({name, channel_fixture::publish(root, source)});
    inputs.push_back({ps::WorkflowInputReference{id}, metadata, structure});
  };
  add("base", {1, 2, 4}, {10, 20, 30, .4f, 11, 21, 31, .6f}, {false, 2});
  add("plane", {1, 2}, {7, 8}, {true, {}});
  add("scalar", {1}, {.5f}, {false, {}, true});
  ps::format::ChannelAssemblyOptions options;
  options.metadata_mode = "raw";
  options.layout = "materialize";
  const auto edge = checked(
      ps::format::replace_channels(document, inputs,
                                   {{{"index", "0"}, {1, {"index", "0"}, {}}},
                                    {{"index", "3"}, {2, {"index", "0"}, {}}}},
                                   options));
  document.outputs = {{"edited", edge.source_node, "values"}};
  ps::Compiler compiler(registry);
  ps::GraphContext graph(document);
  ps::PlanningOptions planning;
  planning.output_regions = {{"edited", ps::Region({{0, 1}, {1, 1}, {0, 4}})}};
  auto compiled = checked(compiler.compile(graph, planning));
  auto result = checked(context.execute(compiled.plan, bindings));
  const auto& value = result.results.at("edited");
  const auto bytes =
      channel_fixture::read(value, ps::Region({{0, 1}, {1, 1}, {0, 4}}));
  const std::array<float, 4> expected{8, 21, 31, .5f};
  for (std::uint64_t c = 0; c < 4; ++c) {
    if (std::memcmp(bytes.data() + c * sizeof(float), &expected[c],
                    sizeof(float)))
      throw std::runtime_error("independent expected-byte check failed");
  }
  std::cout << "offset ROI (0,1,:) = [8, 21, 31, 0.5]; exact bytes verified\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
