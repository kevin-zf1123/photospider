#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
template <class T>
T checked(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
}  // namespace
int main() try {
  ps::WorkflowDocument document;
  ps::ExecutionBindings bindings;
  std::vector<ps::format::ChannelEditInput> inputs;
  const auto add = [&](const std::string& name,
                       const std::vector<std::uint64_t>& shape,
                       const std::vector<float>& samples,
                       ps::format::ChannelEditStructure structure) {
    ps::ValueDescriptor descriptor{ps::ElementType::Float32, shape};
    ps::StridedLayout layout;
    layout.byte_strides.resize(shape.size());
    std::int64_t stride = sizeof(float);
    for (std::size_t i = shape.size(); i-- > 0;) {
      layout.byte_strides[i] = stride;
      stride *= shape[i];
    }
    std::vector<std::uint8_t> bytes(samples.size() * sizeof(float));
    std::memcpy(bytes.data(), samples.data(), bytes.size());
    const auto id = document.inputs.size() + 1;
    const auto region = ps::Region::whole(shape);
    document.inputs.push_back({id, name, descriptor, region, layout, {}});
    bindings.inputs.push_back(
        {name, checked(ps::Value::create(descriptor, region, layout, bytes))});
    ps::OperationMetadata metadata;
    metadata.descriptor = descriptor;
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
  auto registry = ps::make_default_operation_registry();
  ps::Compiler compiler(registry);
  ps::GraphContext graph(document);
  ps::PlanningOptions planning;
  planning.output_regions = {{"edited", ps::Region({{0, 1}, {1, 1}, {0, 4}})}};
  auto compiled = checked(compiler.compile(graph, planning));
  ps::ExecutionContext context(registry);
  auto result = checked(context.execute(compiled.plan, bindings));
  const auto& value = result.values.at("edited");
  const std::array<float, 4> expected{8, 21, 31, .5f};
  for (std::uint64_t c = 0; c < 4; ++c) {
    const auto address = checked(value.byte_address({0, 1, c}));
    if (std::memcmp(value.bytes().data() + address, &expected[c],
                    sizeof(float)))
      throw std::runtime_error("independent expected-byte check failed");
  }
  std::cout << "offset ROI (0,1,:) = [8, 21, 31, 0.5]; exact bytes verified\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
