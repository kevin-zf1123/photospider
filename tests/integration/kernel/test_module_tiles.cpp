#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::take;

struct Module {
  void* handle = dlopen(PS_MODULE_TILES_FIXTURE, RTLD_NOW | RTLD_LOCAL);
  using Counter = std::uint64_t (*)(unsigned);
  Counter count = handle ? reinterpret_cast<Counter>(
                               dlsym(handle, "ps_module_tiles_invocations"))
                         : nullptr;
  ~Module() {
    if (handle)
      dlclose(handle);
  }
  std::array<std::uint64_t, 3> counts() const {
    return {count(0), count(1), count(2)};
  }
};

SchemaTemplate schema() {
  SchemaTemplate schema;
  schema.id = "test.c_tiles";
  ResultTensorSpec tensor;
  tensor.key = "image";
  tensor.descriptor = {ElementType::Float64, {2, 3, 5}};
  tensor.batch_axes = {2, 2};
  tensor.layout.spatial = true;
  tensor.layout.height_axis = 1;
  tensor.layout.width_axis = 2;
  tensor.layout.channel_axis = 0;
  schema.tensors.push_back(std::move(tensor));
  return schema;
}

ExecutionBindings binding(const ResourceBudget& root, std::uint64_t extent,
                          double offset) {
  auto builder =
      take(ResultBuilder::start(root, schema(), "tile.source", {}, extent));
  const auto grid = take(TileGrid::from_schema(schema(), extent));
  for (TileIndex tile = 0; tile < grid.tile_count(); ++tile) {
    const auto region = take(grid.region(tile));
    const auto& dimensions = region.dimensions();
    std::uint64_t count = 1;
    for (const auto& dimension : dimensions)
      count *= dimension.extent;
    std::vector<double> values(count);
    const std::array<std::uint64_t, 5> shape{2, 2, 2, 3, 5};
    for (std::uint64_t i = 0; i < count; ++i) {
      std::uint64_t remainder = i, flat = 0;
      std::array<std::uint64_t, 5> at{};
      for (unsigned axis = 5; axis-- > 0;) {
        at[axis] =
            dimensions[axis].offset + remainder % dimensions[axis].extent;
        remainder /= dimensions[axis].extent;
      }
      for (unsigned axis = 0; axis < 5; ++axis)
        flat = flat * shape[axis] + at[axis];
      values[i] = offset + flat + 1;
    }
    multi_result::check(builder.publish_tensor(
        0, region,
        ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                 values.size() * sizeof(double))));
  }
  return {{{"source", take(builder.seal())}}};
}

// The expected coordinates and values do not use TileGrid or ABI metadata.
int check_samples(const ResultRef& result, std::uint64_t extent,
                  const std::vector<TileIndex>& selected, bool metadata,
                  double offset) {
  const auto facts = take(result.descriptor());
  const std::uint64_t rows = (3 + extent - 1) / extent;
  const std::uint64_t columns = (5 + extent - 1) / extent;
  PS_CHECK(facts.tensor_count() == 1 &&
           result.schema().tensors[0].sample_shape() ==
               std::vector<std::uint64_t>({2, 2, 2, 3, 5}));
  for (std::uint64_t b0 = 0; b0 < 2; ++b0)
    for (std::uint64_t b1 = 0; b1 < 2; ++b1)
      for (std::uint64_t channel = 0; channel < 2; ++channel)
        for (std::uint64_t y = 0; y < 3; ++y)
          for (std::uint64_t x = 0; x < 5; ++x) {
            const auto tile =
                ((b0 * 2 + b1) * rows + y / extent) * columns + x / extent;
            const bool covered = std::find(selected.begin(), selected.end(),
                                           tile) != selected.end();
            double number = -1;
            const auto read = result.read_tensor(
                facts, 0, {b0, b1, channel, y, x}, &number, sizeof(number));
            PS_CHECK(read.ok() == covered);
            if (covered) {
              const double expected =
                  metadata
                      ? 1
                      : offset + (((b0 * 2 + b1) * 2 + channel) * 3 + y) * 5 +
                            x + 1;
              PS_CHECK(number == expected);
            } else {
              PS_CHECK(read.code == ErrorCode::InvalidArgument && number == -1);
            }
          }
  return 0;
}

// Source kinds: direct generator, external binding, generated input,
// schema-only.
int execute_tiles(const Module& module,
                  const std::shared_ptr<OperationRegistry>& registry,
                  std::uint64_t extent, unsigned source) {
  WorkflowDocument document;
  if (source == 0) {
    document.nodes = {{1, "test.c_tiles.generate", {}, {}}};
  } else {
    WorkflowInput input = WorkflowNodeOutput{2, "tiled"};
    if (source == 1) {
      document.inputs = {multi_result::declaration(1, "source", schema())};
      input = WorkflowInputReference{1};
    } else {
      document.nodes = {{2, "test.c_tiles.generate", {}, {}}};
    }
    document.nodes.push_back(
        {1,
         source == 3 ? "test.c_tiles.schema" : "test.c_tiles.copy",
         {input},
         {}});
  }
  document.outputs = {{"result", 1, "tiled"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.tile_extent = extent;
  const auto full =
      take(Compiler(registry).compile(graph, planning)).plan.value();
  // All layout and original tile identities must survive artifact rebinding.
  const auto serialized = take(CompilerArtifact::export_plan(full));
  const auto imported =
      take(CompilerArtifact::import_plan(registry, serialized));
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.managed_resources = ResourceLimits{};
  ExecutionContext execution(registry, config);
  const auto root = take(execution.resource_budget());
  const std::uint64_t rows = (3 + extent - 1) / extent;
  const std::uint64_t columns = (5 + extent - 1) / extent;
  const std::uint64_t count = 4 * rows * columns;
  for (unsigned selection = 0; selection < 3; ++selection) {
    std::vector<TileIndex> tiles;
    if (selection == 0) {
      for (TileIndex tile = 0; tile < count; ++tile)
        tiles.push_back(tile);
    } else if (selection == 1) {
      tiles = extent == 2 ? std::vector<TileIndex>{5, 18, 23}
                          : std::vector<TileIndex>{0, count / 2, count - 1};
    }
    const TileRequest requested{
        {{0, take(TileSet::from_indices(root, tiles))}}};
    const auto plan = take(imported.select_outputs({{"result", requested}}));
    const auto expected_tasks = tiles.size() * (source == 2 ? 2 : 1);
    PS_CHECK(plan.program().tasks.size() == expected_tasks);
    if (source == 3)
      for (const auto& task : plan.program().tasks)
        PS_CHECK(task.reads.empty());
    auto prepared = take(execution.prepare(plan));
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
      const auto before = module.counts();
      const double offset = source == 1 ? 100 * (repeat + 1) : 0;
      auto bindings =
          source == 1 ? binding(root, extent, offset) : ExecutionBindings{};
      auto run = take(execution.submit(prepared, std::move(bindings)));
      auto result = take(run.join());
      const auto after = module.counts();
      PS_CHECK(result.diagnostics.completed_tasks == expected_tasks &&
               result.results.size() == 1);
      PS_CHECK(check_samples(result.results.at("result"), extent, tiles,
                             source == 3, offset) == 0);
      PS_CHECK(after[0] - before[0] ==
               (source == 0 || source == 2 ? tiles.size() : 0));
      PS_CHECK(after[1] - before[1] ==
               (source == 1 || source == 2 ? tiles.size() : 0));
      PS_CHECK(after[2] - before[2] == (source == 3 ? tiles.size() : 0));
    }
  }
  const auto before = module.counts();
  const TileRequest bad_tile{{{0, take(TileSet::from_indices(root, {count}))}}};
  auto invalid = imported.select_outputs({{"result", bad_tile}});
  PS_CHECK(!invalid.ok() &&
           invalid.status().code == ErrorCode::InvalidArgument);
  const TileRequest bad_member{{{1, take(TileSet::from_indices(root, {0}))}}};
  invalid = imported.select_outputs({{"result", bad_member}});
  PS_CHECK(!invalid.ok() &&
           invalid.status().code == ErrorCode::InvalidArgument);
  PS_CHECK(module.counts() == before);
  return 0;
}
}  // namespace

int main() try {
  Module module;
  PS_CHECK(module.handle && module.count);
  auto registry = std::make_shared<OperationRegistry>();
  PS_REQUIRE_OK(registry->load_plugin(PS_MODULE_TILES_FIXTURE));
  PS_REQUIRE_OK(registry->freeze());
  for (std::uint64_t extent : {1, 2, 4, 128})
    for (unsigned source = 0; source < 4; ++source)
      PS_CHECK(execute_tiles(module, registry, extent, source) == 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
