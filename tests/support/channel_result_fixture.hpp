#pragma once

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "channel_extraction_workflow/source.hpp"

namespace assembly_fixture {
using namespace ps;  // NOLINT(build/namespaces)
using channel_fixture::take;
inline void require(bool okay, const std::string& message) {
  if (!okay)
    throw std::runtime_error(message);
}
inline const ResultTensorSpec& spec(const ResultRef& result) {
  return result.schema().tensors[0];
}
inline const std::vector<ValueFacet>& facets(const ResultRef& result) {
  return spec(result).facets;
}
inline const void* owner(const ResultRef& result) {
  auto descriptor = take(result.descriptor());
  const auto& box = descriptor.tensor_coverage(0).boxes().front();
  return channel_fixture::owner(result, box);
}
inline std::vector<std::uint8_t> read(const ResultRef& result,
                                      const Region& region) {
  return channel_fixture::read(result, region);
}
struct Fixture final {
  ResourceBudget root;
  ExecutionContextConfig execution_config;
  WorkflowDocument document;
  ExecutionBindings bindings;
  std::vector<std::vector<std::uint8_t>> raw;
  std::vector<ValueDescriptor> descriptors;
  std::vector<channel_fixture::Source> sources;
  WorkflowInput add_source(channel_fixture::Source source) {
    const auto id = document.inputs.size() + 1;
    const auto name = "input" + std::to_string(id);
    auto declaration = channel_fixture::declaration(source);
    declaration.id = id;
    declaration.name = name;
    document.inputs.push_back(std::move(declaration));
    bindings.inputs.push_back({name, channel_fixture::publish(root, source)});
    raw.push_back(source.bytes);
    descriptors.push_back(source.schema.tensors[0].descriptor);
    sources.push_back(std::move(source));
    return WorkflowInputReference{id};
  }
  WorkflowInput add(ValueDescriptor descriptor,
                    std::vector<ValueFacet> facets = {},
                    ResourceBindings resources = {}) {
    auto source =
        channel_fixture::source(std::move(descriptor), std::move(facets));
    const auto id = document.inputs.size() + 1;
    for (std::size_t i = 0; i < source.bytes.size(); ++i)
      source.bytes[i] = (i * 73 + id * 37) & 255;
    source.resources = std::move(resources);
    return add_source(std::move(source));
  }
  WorkflowInput image(ValueDescriptor descriptor, PlanarImageConfig config,
                      const std::vector<Region>& published = {}) {
    ResultTensorLayout layout;
    layout.spatial = true;
    layout.order = config.order;
    layout.height_axis = config.height_axis;
    layout.width_axis = config.width_axis;
    layout.channel_axis = config.channel_axis;
    layout.row_pitch_bytes = config.row_pitch_bytes;
    layout.groups = config.groups;
    auto source = channel_fixture::source(std::move(descriptor), {}, layout);
    const auto id = document.inputs.size() + 1;
    for (std::size_t i = 0; i < source.bytes.size(); ++i)
      source.bytes[i] = (i * 73 + id * 37) & 255;
    if (!published.empty())
      source.coverage = published;
    return add_source(std::move(source));
  }
  void bind_to(ExecutionContext& context) {
    root = take(context.resource_budget());
    for (std::size_t i = 0; i < sources.size(); ++i)
      rebind(i);
  }
  void rebind(std::size_t index) {
    sources[index].bytes = raw[index];
    bindings.inputs[index].result =
        channel_fixture::publish(root, sources[index]);
  }
  OperationMetadata metadata(std::size_t index) const {
    OperationMetadata metadata;
    metadata.result_schema = document.inputs[index].result_schema;
    return metadata;
  }
};
inline ExecutionResult run(Fixture& fixture, WorkflowNodeOutput output,
                           const std::optional<Region>& roi = {},
                           std::shared_ptr<OperationRegistry> registry = {}) {
  fixture.document.outputs = {
      {"result", output.source_node, output.source_port}};
  if (!registry)
    registry = make_default_operation_registry();
  GraphContext graph(fixture.document);
  PlanningOptions planning;
  if (roi)
    planning.output_regions = {{"result", *roi}};
  ResourceBindings resources;
  for (const auto& input : fixture.bindings.inputs)
    resources = take(resources.unite(input.result.resources()));
  auto compiled = take(Compiler(registry).compile(graph, planning, resources));
  auto config = fixture.execution_config;
  config.cpu_workers = 1;
  ExecutionContext context(registry, config);
  fixture.bind_to(context);
  return take(context.execute(compiled.plan, fixture.bindings));
}
template <class Run>
std::uint64_t source_bytes(const Run& run, const Fixture& fixture) {
  const auto support = take(run.dependencies.source_support());
  std::uint64_t bytes = 0;
  for (std::size_t i = 0; i < fixture.bindings.inputs.size(); ++i) {
    const auto found = support.find(fixture.bindings.inputs[i].name);
    if (found != support.end())
      bytes += take(found->second.element_count()) *
               Value::element_size(fixture.descriptors[i].element_type);
  }
  return bytes;
}
template <class Run>
void check_oracle(const Run& run, const Fixture& fixture,
                  std::uint32_t output_axis,
                  const std::vector<std::optional<std::uint32_t>>& source_axes,
                  const std::vector<std::pair<unsigned, unsigned>>& mapping,
                  const Region& requested) {
  const auto& result = run.results.at("result");
  const auto& descriptor = spec(result).descriptor;
  const auto width = Value::element_size(descriptor.element_type);
  const auto packed = read(result, requested);
  auto samples =
      take(Footprint::from_regions(spec(result).sample_shape(), {requested}));
  std::size_t linear = 0;
  auto status = samples.visit(
      [&](const auto& coordinate) {
        const auto selected = mapping.at(coordinate[output_axis]);
        auto at = coordinate;
        at.erase(at.begin() + output_axis);
        if (source_axes[selected.first])
          at.insert(at.begin() + *source_axes[selected.first], selected.second);
        else if (fixture.descriptors[selected.first].shape ==
                 std::vector<std::uint64_t>{1})
          at = {0};
        std::uint64_t source = 0;
        for (std::size_t i = 0; i < at.size(); ++i)
          source = source * fixture.sources[selected.first]
                                .schema.tensors[0]
                                .sample_shape()[i] +
                   at[i];
        require(!std::memcmp(
                    packed.data() + linear * width,
                    fixture.raw[selected.first].data() + source * width, width),
                "independent byte oracle");
        ++linear;
        return Status::success();
      },
      UINT64_MAX);
  require(status.ok(), "oracle traversal");
}
}  // namespace assembly_fixture
