#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "08-transform/stmap_result.hpp"
#include "support/multi_output_result_fixture.hpp"

namespace stmap_test {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
inline std::shared_ptr<OperationRegistry> registry() {
  auto result = std::make_shared<OperationRegistry>();
  check(result->register_operation(plugin_internal::stmap_result::operation()));
  check(result->freeze());
  return result;
}
inline SchemaTemplate image_schema(std::uint64_t height, std::uint64_t width,
                                   std::uint64_t frames = 1) {
  auto schema = plugin_internal::image_ops::image_schema();
  schema.tensors[0].descriptor.shape = {height, width, 4};
  schema.tensors[0].batch_axes = {frames, 1};
  return schema;
}
inline Status read(const ResultRef& result,
                   const std::vector<std::uint64_t>& at, void* destination,
                   std::size_t bytes) {
  auto facts = result.descriptor();
  return facts.ok()
             ? result.read_tensor(facts.value(), 0, at, destination, bytes)
             : facts.status();
}
struct StmapDriver {
  std::shared_ptr<OperationRegistry> operations;
  ExecutionContext context;
  ResourceBudget root;
  std::unique_ptr<GraphContext> graph;
  explicit StmapDriver(std::shared_ptr<OperationRegistry> selected = registry(),
                       ExecutionContextConfig config = {})
      : operations(std::move(selected)),
        context(operations, config),
        root(take(context.resource_budget())) {}
  template <class T>
  ResultRef input(const SchemaTemplate& schema, const std::vector<T>& values) {
    auto builder =
        take(ResultBuilder::start(root, schema, "stmap.input", {}, {}, 4));
    check(builder.publish_tensor(
        0, Region::whole(schema.tensors[0].sample_shape()),
        ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                 values.size() * sizeof(T))));
    return take(builder.seal());
  }
  ResultRef source(const SchemaTemplate& schema,
                   const std::vector<float>& pixels) {
    return input(schema, pixels);
  }
  ResultRef map(const std::vector<double>& values,
                std::vector<std::uint64_t> shape = {1, 1, 2},
                bool spatial = false, std::vector<std::uint64_t> batches = {}) {
    auto schema = multi_result::schema(ElementType::Float64, std::move(shape));
    schema.tensors[0].layout.spatial = spatial;
    schema.tensors[0].batch_axes.assign(batches.begin(), batches.end());
    return input(schema, values);
  }
  ExecutionPlan plan(const ResultRef& source, const ResultRef& map,
                     const std::string& boundary = "clamp",
                     std::uint64_t extent = 2) {
    WorkflowDocument document;
    document.inputs = {multi_result::declaration(1, "source", source.schema()),
                       multi_result::declaration(2, "map", map.schema())};
    document.nodes = {{1,
                       "image.stmap",
                       {WorkflowInputReference{1}, WorkflowInputReference{2}},
                       {{"boundary", boundary}}}};
    document.outputs = {{"result", 1, "value"}};
    graph = std::make_unique<GraphContext>(std::move(document));
    PlanningOptions planning;
    planning.tile_extent = extent;
    return take(Compiler(operations).compile(*graph, planning)).plan;
  }
  Result<DemandResult> run(ResultRef source, ResultRef map,
                           const std::string& boundary, const Footprint& output,
                           ExecutionOptions options = {},
                           const CancellationToken& cancellation = {},
                           const PlanningOptions& planning = {}) try {
    auto compiled = plan(source, map, boundary, planning.tile_extent);
    auto frozen =
        context.freeze(compiled, {{{"source", source}, {"map", map}}});
    if (!frozen.ok())
      return Result<DemandResult>(frozen.status());
    auto tiles =
        sdk::tiles_for_samples(root, *compiled.steps()[0].tile_grid, output);
    if (!tiles.ok())
      return Result<DemandResult>(tiles.status());
    return context.execute_fragments(frozen.value(),
                                     {{"result", {false, tiles.take_value()}}},
                                     cancellation, options);
  } catch (const multi_result::Failure& failure) {
    return Result<DemandResult>(failure.status);
  }
  std::map<std::string, TileRequest> request(std::vector<TileIndex> indices) {
    return {{"result", {false, take(TileSet::from_indices(root, indices))}}};
  }
};
}  // namespace stmap_test
