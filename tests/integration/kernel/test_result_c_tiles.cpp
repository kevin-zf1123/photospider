#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/plugin/result_tile_helpers.h"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::take;
SchemaTemplate input_schema(bool spatial = true) {
  auto schema = multi_result::schema(ElementType::Float64, {1, 5, 2});
  auto& tensor = schema.tensors[0];
  tensor.batch_axes = {2, 2};
  tensor.layout.spatial = spatial;
  tensor.layout.height_axis = 0;
  tensor.layout.width_axis = 1;
  return schema;
}
ExecutionBindings bindings(const ResourceBudget& root, bool spatial) {
  auto schema = input_schema(spatial);
  schema.tensors[0].layout.order = ImagePlaneOrder::Continuous;
  schema.tensors[0].layout.row_pitch_bytes = 128;
  auto builder = take(ResultBuilder::start(root, schema, "c.tiles.source"));
  std::vector<double> values(40);
  for (std::size_t i = 0; i < values.size(); ++i)
    values[i] = i + 1;
  multi_result::check(builder.publish_tensor(
      0, Region::whole({2, 2, 1, 5, 2}),
      ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
               values.size() * sizeof(double))));
  return {{{"source", take(builder.seal())}}};
}
int run(bool producer, bool whole, bool spatial = true) {
  struct Module {
    void* handle = dlopen(PS_RESULT_TILE_NEED_FIXTURE, RTLD_NOW | RTLD_LOCAL);
    ~Module() {
      if (handle)
        dlclose(handle);
    }
  } module;
  PS_CHECK(module.handle);
  auto set_hook = reinterpret_cast<void (*)(void (*)(void*, int), void*)>(
      dlsym(module.handle, "ps_result_tile_need_observe"));
  PS_CHECK(set_hook);
  auto registry = std::make_shared<OperationRegistry>();
  multi_result::check(registry->load_plugin(PS_RESULT_TILE_NEED_FIXTURE));
  std::atomic<unsigned> starts{0};
  OperationDefinition upstream;
  upstream.key = "test.c_tiles.upstream";
  upstream.traits.outputs = {
      multi_result::output("value", input_schema(spatial))};
  upstream.traits.outputs[0].region_rule =
      whole ? OperationRegionRule::Whole : OperationRegionRule::Tiled;
  upstream.traits.workspace_bytes = 4096;
  upstream.traits.cacheable = false;
  upstream.start_result = [&](const auto&, const auto& allocator) {
    ++starts;
    return ResultContinuation::make<multi_result::Program>(allocator, -1, 7);
  };
  multi_result::check(registry->register_operation(std::move(upstream)));
  multi_result::check(registry->freeze());
  ExecutionContextConfig config;
  config.result_cache_bytes = 0;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->maximum_work = 10000000;
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  for (std::int64_t mode = spatial ? 0 : 25; mode <= (spatial ? 28 : 27);
       ++mode) {
    if (spatial && mode >= 25 && mode <= 27)
      continue;
    WorkflowDocument document;
    WorkflowInput source = WorkflowInputReference{1};
    if (producer) {
      document.nodes = {{1, "test.c_tiles.upstream", {}, {}}};
      source = WorkflowNodeOutput{1, "value"};
    } else {
      document.inputs = {
          multi_result::declaration(1, "source", input_schema(spatial))};
    }
    document.nodes.push_back(
        {2, "test.c_tile_need", {source}, {{"mode", mode}}});
    document.outputs = {{"result", 2, mode == 14 ? "independent" : "value"}};
    GraphContext graph(document);
    PlanningOptions options;
    options.tile_extent = 2;
    auto plan = take(Compiler(registry).compile(graph, options)).plan;
    auto bound = producer ? ExecutionBindings{} : bindings(root, spatial);
    auto demand = take(context.open_demand(plan, bound));
    const auto before = starts.load();
    struct Cancellation {
      CancellationSource source;
      std::atomic<int> status{-1};
      bool cancel = false;
      ResourceBudget root;
      std::uint64_t maximum_work = 0;
      std::atomic<bool> pressure_admitted{false};
    } cancellation;
    cancellation.cancel = mode == 23;
    cancellation.root = root;
    cancellation.maximum_work = config.managed_resources->maximum_work;
    set_hook(
        [](void* raw, int status) {
          auto& observed = *static_cast<Cancellation*>(raw);
          if (status == -1) {
            const auto issued = observed.root.statistics().issued.work;
            if (issued <= observed.maximum_work - 64)
              observed.pressure_admitted =
                  observed.root.consume({observed.maximum_work - 64 - issued})
                      .ok();
            return;
          }
          observed.status = status;
          if (observed.cancel)
            observed.source.cancel();
        },
        &cancellation);
    auto result =
        demand.request({{"result", {true, {}}}}, cancellation.source.token());
    set_hook(nullptr, nullptr);
    if (mode == 23)
      PS_CHECK(cancellation.status == 6 &&
               cancellation.source.token().cancelled());
    if (mode == 28)
      PS_CHECK(cancellation.pressure_admitted && cancellation.status == 4);
    const bool success =
        mode <= 3 || mode == 16 || mode == 22 || mode == 26 || mode == 27;
    if (!success) {
      PS_CHECK(!result.ok() && result.status().code ==
                                   (mode == 28 ? ErrorCode::ResourceExhausted
                                               : ErrorCode::InvalidArgument));
      PS_CHECK(starts.load() == before);
      if (mode != 20 && mode != 21 && mode != 28)
        PS_CHECK(result.status().detail.origin == FailureOrigin::Protocol &&
                 result.status().detail.scope == FailureScope::Group);
      if (mode == 13)
        PS_CHECK(result.status().reason == FailureReason::UnauthorizedRead);
      continue;
    }
    if (!result.ok())
      std::cerr << "C tiles mode " << mode << ": " << result.status().message
                << '\n';
    PS_CHECK(result.ok());
    const double expected = (mode == 3 || mode == 27)   ? 1
                            : producer                  ? 7
                            : (mode == 2 || mode == 26) ? 40
                            : mode == 22                ? 27
                                                        : 21;
    PS_CHECK(multi_result::number(result.value().results.at("result")) ==
             expected);
    const auto step = plan.outputs().at("result");
    auto record = take(result.value().dependencies.tile_record(
        {step, plan.steps()[step].output_index, 0}));
    PS_CHECK(record && record->inputs.size() == 1);
    const auto& need = record->inputs[0];
    if (mode == 2 || mode == 26 ||
        (producer && whole && mode != 3 && mode != 27)) {
      PS_CHECK(need.whole && need.tiles.empty());
    } else if (mode == 3 || mode == 27) {
      PS_CHECK(!need.whole && need.tiles.empty());
    } else {
      const std::vector<TileIndex> wanted =
          mode == 1    ? std::vector<TileIndex>{6}
          : mode == 22 ? std::vector<TileIndex>{1, 2, 4, 5, 7, 8, 10, 11}
                       : std::vector<TileIndex>{6, 7};
      PS_CHECK(
          !need.whole && need.tiles.size() == wanted.size() &&
          std::equal(need.tiles.begin(), need.tiles.end(), wanted.begin()));
    }
    if (producer) {
      const unsigned computed = (mode == 3 || mode == 27) ? 0
                                : whole                   ? 1
                                : mode == 2               ? 12
                                : mode == 1               ? 1
                                : mode == 22              ? 8
                                                          : 2;
      PS_CHECK(starts.load() - before == computed);
    }
  }
  return 0;
}
int output_queries() {
  struct Module {
    void* handle = dlopen(PS_RESULT_TILE_NEED_FIXTURE, RTLD_NOW | RTLD_LOCAL);
    ~Module() {
      if (handle)
        dlclose(handle);
    }
  } module;
  PS_CHECK(module.handle);
  auto starts = reinterpret_cast<std::uint64_t (*)(std::uint32_t)>(
      dlsym(module.handle, "ps_result_tile_query_starts"));
  PS_CHECK(starts);
  auto registry = std::make_shared<OperationRegistry>();
  multi_result::check(registry->load_plugin(PS_RESULT_TILE_NEED_FIXTURE));
  multi_result::check(registry->freeze());
  ExecutionContextConfig config;
  config.result_cache_bytes = 0;
  ExecutionContext execution(registry, config);
  auto root = take(execution.resource_budget());
  const char* ports[] = {"tiled", "whole", "bundle"};
  for (std::uint32_t output = 0; output < 3; ++output) {
    WorkflowDocument doc;
    doc.nodes = {{1, "test.c_tile_query", {}, {}}};
    doc.outputs = {{"result", 1, ports[output]}};
    GraphContext graph(doc);
    PlanningOptions planning;
    planning.tile_extent = 2;
    auto plan = take(Compiler(registry).compile(graph, planning)).plan;
    auto demand = take(execution.open_demand(plan, {}));
    const auto step = plan.outputs().at("result");
    PS_CHECK(plan.steps()[step].tiled == (output == 0));
    for (bool all : {false, true}) {
      const std::vector<TileIndex> selected{5, 18, 23};
      const TileRequest request =
          all || output == 2
              ? TileRequest{true, {}}
              : TileRequest{false, take(TileSet::from_indices(root, selected))};
      const auto before = starts(output);
      auto result = take(demand.request({{"result", request}}));
      const auto& value = result.results.at("result");
      const auto facts = take(value.descriptor());
      const bool complete = all || output != 0;
      for (std::uint64_t b0 = 0; b0 < 2; ++b0)
        for (std::uint64_t b1 = 0; b1 < 2; ++b1)
          for (std::uint64_t channel = 0; channel < 2; ++channel)
            for (std::uint64_t y = 0; y < 3; ++y)
              for (std::uint64_t x = 0; x < 5; ++x) {
                const auto tile = ((b0 * 2 + b1) * 2 + y / 2) * 3 + x / 2;
                const bool covered =
                    complete || std::find(selected.begin(), selected.end(),
                                          tile) != selected.end();
                double number = 0;
                const auto read = value.read_tensor(
                    facts, 0, {b0, b1, channel, y, x}, &number, sizeof(number));
                PS_CHECK(read.ok() == covered);
                if (covered)
                  PS_CHECK(number ==
                           (((b0 * 2 + b1) * 2 + channel) * 3 + y) * 5 + x + 1);
              }
      if (output == 2) {
        double number = 0;
        PS_CHECK(
            value.read_tensor(facts, 1, {0}, &number, sizeof(number)).ok() &&
            number == 42);
      }
      const auto units = output == 0 ? 24U : 1U;
      for (TileIndex tile = 0; tile < units; ++tile) {
        const bool covered =
            complete ||
            std::find(selected.begin(), selected.end(), tile) != selected.end();
        auto record =
            take(result.dependencies.tile_record({step, output, tile}));
        PS_CHECK(static_cast<bool>(record) == covered);
        if (record)
          PS_CHECK(record->inputs.empty());
      }
      PS_CHECK(starts(output) - before ==
               (output == 0 ? (all ? 24U : 3U) : 1U));
    }
    const auto before = starts(output);
    auto empty = take(demand.request({{"result", {false, {}}}}));
    auto facts = take(empty.results.at("result").descriptor());
    for (std::uint32_t slot = 0; slot < facts.tensor_count(); ++slot)
      PS_CHECK(facts.tensor_coverage(slot).empty());
    PS_CHECK(starts(output) == before);
    auto invalid = demand.request(
        {{"result", {false, take(TileSet::from_indices(root, {24}))}}});
    PS_CHECK(!invalid.ok() &&
             invalid.status().code == ErrorCode::InvalidArgument &&
             starts(output) == before);
  }
  return 0;
}
int direct_queries() {
  std::shared_ptr<void> module(
      dlopen(PS_RESULT_TILE_NEED_FIXTURE, RTLD_NOW | RTLD_LOCAL),
      [](void* handle) {
        if (handle)
          dlclose(handle);
      });
  PS_CHECK(module);
  auto starts = reinterpret_cast<std::uint64_t (*)(std::uint32_t)>(
      dlsym(module.get(), "ps_result_tile_query_starts"));
  PS_CHECK(starts);
  ResourceBudget root;
  ResourceAllocationScope scope(root);
  auto registry = std::make_shared<OperationRegistry>();
  multi_result::check(registry->load_plugin(PS_RESULT_TILE_NEED_FIXTURE));
  multi_result::check(registry->freeze());
  const std::map<std::string, ParameterValue> parameters;
  auto prepared =
      take(registry->prepare_operation("test.c_tile_query", {}, parameters));
  auto outputs =
      take(infer_operation_outputs(prepared->traits(), {}, parameters));
  auto allocator = root.allocator();
  for (unsigned mode = 0; mode < 12; ++mode) {
    const bool whole = mode >= 4 && mode != 11;
    ResultProgramMetadata metadata{{}, outputs[whole ? 1 : 0]};
    metadata.output_tile_grid =
        take(TileGrid::from_schema(*metadata.output.result_schema, 2));
    ResultProgramQuery query(metadata, parameters);
    query.output_index = whole ? 1 : 0;
    query.tile_extent = 2;
    query.prepared = prepared;
    query.semantic_key = "direct.c.tile.query";
    if (mode == 11) {
      query.output_request = {false, take(TileSet::from_indices(root, {5}))};
      query.tile_unit = 5;
    }
    if (mode == 9 || mode == 10) {
      auto different = *metadata.output.result_schema;
      if (mode == 9)
        different.tensors[0].descriptor.shape[2] = 9;
      metadata.output_tile_grid =
          take(TileGrid::from_schema(different, mode == 10 ? 4 : 2));
      query.output_request = {
          false, take(TileSet::from_indices(root, {mode == 9 ? 25U : 5U}))};
    }
    if (mode == 1) {
      query.output_request = {false, take(TileSet::from_indices(root, {5, 6}))};
      query.tile_unit = 5;
    } else if (mode == 2 || mode == 4) {
      query.output_request = {false, take(TileSet::from_indices(root, {24}))};
      query.tile_unit = whole ? 0 : 24;
    } else if (mode == 3 || mode == 5 || mode == 6) {
      query.output_request = {false, take(TileSet::from_indices(root, {5}))};
      query.tile_unit = mode == 3 ? 6 : mode == 5 ? 0 : 5;
    } else if (mode == 7) {
      query.output_request = {true, take(TileSet::from_indices(root, {5}))};
    }
    auto continuation =
        take(registry->start_result("test.c_tile_query", query, allocator));
    ResultObjectInputs objects;

    ResultProgramPhase phase{query,
                             objects,
                             allocator,
                             root,
                             [&](std::uint64_t n) { return root.consume({n}); },
                             {}};
    const auto before = starts(query.output_index);
    auto result = continuation.poll(phase);
    const std::uint64_t expected_calls =
        mode == 5 || mode == 8 || mode == 11 ? 1 : 0;
    PS_CHECK(starts(query.output_index) - before == expected_calls);
    if (mode == 11) {
      PS_CHECK(result.ok() &&
               std::holds_alternative<ResultPublication>(result.value()));
      auto value = std::get<ResultPublication>(result.value()).result;
      PS_CHECK(multi_result::number(value, {0, 0, 1, 2, 4}) == 30);
      PS_CHECK(
          take(take(value.descriptor()).tensor_coverage(0).element_count()) ==
          2);
    } else if (mode == 5 || mode == 8) {
      PS_CHECK(result.ok() &&
               std::holds_alternative<ResultPublication>(result.value()));
      auto value = std::get<ResultPublication>(result.value()).result;
      PS_CHECK(multi_result::number(value, {1, 1, 1, 2, 4}) == 120);
    } else {
      PS_CHECK(!result.ok() &&
               result.status().code == ErrorCode::InvalidArgument);
    }
  }
  return 0;
}
int direct_input_grids() {
  struct Module {
    void* handle = dlopen(PS_RESULT_TILE_NEED_FIXTURE, RTLD_NOW | RTLD_LOCAL);
    ~Module() {
      if (handle)
        dlclose(handle);
    }
  } module;
  PS_CHECK(module.handle);
  auto starts = reinterpret_cast<std::uint64_t (*)()>(
      dlsym(module.handle, "ps_result_tile_need_starts"));
  PS_CHECK(starts);
  ResourceBudget root;
  ResourceAllocationScope scope(root);
  auto registry = std::make_shared<OperationRegistry>();
  multi_result::check(registry->load_plugin(PS_RESULT_TILE_NEED_FIXTURE));
  multi_result::check(registry->freeze());
  const std::map<std::string, ParameterValue> parameters{
      {"mode", std::int64_t{3}}};
  for (unsigned mode = 0; mode < 8; ++mode) {
    auto schema = input_schema();
    const auto valid_grid = take(TileGrid::from_schema(schema, 2));
    if (mode == 7) {
      auto other = schema.tensors[0];
      other.key = "other";
      schema.tensors.push_back(std::move(other));
    }
    OperationMetadata input;
    input.result_schema = std::make_shared<const SchemaTemplate>(schema);
    std::vector<OperationMetadata> inputs{input};
    auto prepared = take(
        registry->prepare_operation("test.c_tile_need", inputs, parameters));
    auto outputs =
        take(infer_operation_outputs(prepared->traits(), inputs, parameters));
    ResultProgramMetadata metadata{inputs, outputs[0]};
    if (mode)
      metadata.input_tile_grids = {valid_grid};
    if (mode == 2)
      metadata.input_tile_grids[0].reset();
    if (mode == 3) {
      schema.tensors[0].descriptor.shape[1] = 9;
      metadata.input_tile_grids[0] = take(TileGrid::from_schema(schema, 2));
    }
    if (mode == 4)
      metadata.input_tile_grids[0] = take(TileGrid::from_schema(schema, 4));
    if (mode == 5)
      metadata.input_tile_grids.push_back(valid_grid);
    if (mode == 6)
      metadata.output_tile_grid = valid_grid;
    ResultProgramQuery query(metadata, parameters);
    query.tile_extent = 2;
    query.prepared = prepared;
    query.semantic_key = "direct.c.input.grid";
    auto allocator = root.allocator();
    auto continuation =
        take(registry->start_result("test.c_tile_need", query, allocator));
    ResultObjectInputs objects;

    ResultProgramPhase phase{query,
                             objects,
                             allocator,
                             root,
                             [&](std::uint64_t n) { return root.consume({n}); },
                             {}};
    const auto before = starts();
    auto result = continuation.poll(phase);
    if (mode < 2) {
      PS_CHECK(result.ok() &&
               std::holds_alternative<ResultProgramNeed>(result.value()) &&
               starts() == before + 1);
      const auto& needs = std::get<ResultProgramNeed>(result.value()).tiles;
      PS_CHECK(needs.size() == 1 && needs[0].descriptor_only);
    } else {
      PS_CHECK(!result.ok() &&
               result.status().code == ErrorCode::InvalidArgument &&
               starts() == before);
    }
  }
  return 0;
}
int malformed_sdk_grid() {
  ps_result_tensor_spec_v3 tensor{};
  tensor.struct_size = sizeof(tensor);
  tensor.rank = 2;
  tensor.shape[0] = tensor.shape[1] = 4;
  tensor.spatial = 1;
  tensor.height_axis = 0;
  tensor.width_axis = 1;
  ps_result_schema_v3 schema{};
  schema.struct_size = sizeof(schema);
  schema.tensor_count = 1;
  schema.tensors = &tensor;
  ps_result_output_v3 output{};
  output.struct_size = sizeof(output);
  output.port.schema = &schema;
  output.execution = PS_RESULT_REGIONAL_V3;
  ps_result_tile_grid_v3 grid{};
  grid.struct_size = sizeof(grid);
  grid.present = 1;
  grid.rank = 2;
  grid.width_axis = 1;
  grid.tile_extent = grid.tile_rows = grid.tile_columns = 2;
  grid.tile_count = grid.shape[0] = grid.shape[1] = 4;
  const std::uint64_t tile = 1;
  ps_result_query_v3 query{};
  query.struct_size = sizeof(query);
  query.tiled = 1;
  query.tile_unit = tile;
  query.tile_extent = 2;
  query.output = &output;
  query.output_tile_grid = &grid;
  query.requested = {sizeof(query.requested), 0, &tile, 1};
  ps_result_region_v3 region{};
  PS_CHECK(ps_result_output_region_v3(&query, 0, &region) == 0 &&
           region.offset[0] == 0 && region.offset[1] == 2 &&
           region.extent[0] == 2 && region.extent[1] == 2);
  for (unsigned mode = 0; mode < 7; ++mode) {
    auto bad = grid;
    if (mode == 0) {
      bad.tile_rows = 4;
      bad.tile_columns = 1;
    } else if (mode == 1) {
      bad.height_axis = 1;
      bad.width_axis = 0;
    } else if (mode == 2) {
      bad.tile_count = 8;
    } else if (mode == 3) {
      bad.tile_extent = 4;
    } else if (mode == 4) {
      schema.field_count = 1;
    } else if (mode == 5) {
      schema.tensor_count = 2;
    } else {
      tensor.spatial = 0;
    }
    query.output_tile_grid = &bad;
    region.struct_size = 17;
    region.rank = 9;
    std::fill(std::begin(region.offset), std::end(region.offset), 66);
    std::fill(std::begin(region.extent), std::end(region.extent), 77);
    PS_CHECK(ps_result_output_region_v3(&query, 0, &region) ==
             PS_RESULT_STATUS_INVALID_ARGUMENT_V3);
    PS_CHECK(region.struct_size == 17 && region.rank == 9);
    for (unsigned axis = 0; axis < 8; ++axis)
      PS_CHECK(region.offset[axis] == 66 && region.extent[axis] == 77);
    schema.field_count = 0;
    schema.tensor_count = 1;
    tensor.spatial = 1;
  }
  query.output_tile_grid = &grid;
  output.execution = PS_RESULT_WHOLE_V3;
  query.tiled = 0;
  query.requested = {sizeof(query.requested), 0, nullptr, 0};
  PS_CHECK(ps_result_output_region_v3(&query, 0, &region) ==
           PS_RESULT_STATUS_INVALID_ARGUMENT_V3);
  PS_CHECK(region.struct_size == 17 && region.rank == 9);
  for (unsigned axis = 0; axis < 8; ++axis)
    PS_CHECK(region.offset[axis] == 66 && region.extent[axis] == 77);
  query.tile_unit = 0;
  PS_CHECK(ps_result_output_region_v3(&query, 0, &region) == 0 &&
           region.rank == 2 && region.extent[0] == 0 && region.extent[1] == 0);
  return 0;
}
}  // namespace
int main() try {
  return run(false, false) || run(true, false) || run(true, true) ||
         run(false, false, false) || run(true, true, false) ||
         output_queries() || direct_queries() || direct_input_grids() ||
         malformed_sdk_grid();
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
