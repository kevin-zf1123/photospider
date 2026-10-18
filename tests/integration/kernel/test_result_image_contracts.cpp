#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "photospider/plugin/whole_stage.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/unified_result_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}
void zero(const ResourceBudget& root) {
  for (auto count : root.statistics().live.values)
    require(!count, "last owner capacity");
}
ResultRef image(const ResourceBudget& root, const SchemaTemplate& schema,
                float bias = 0, uint32_t biased_slot = UINT32_MAX) {
  auto made = ResultBuilder::start(root, schema, "source", {}, 1);
  require(made.ok(), made.status().message);
  auto builder = made.take_value();

  for (uint32_t slot = 0; slot < schema.tensors.size(); ++slot) {
    auto count = schema.tensors[slot].sample_count().value();
    std::vector<float> samples(count);
    for (uint64_t i = 0; i < count; ++i)
      samples[i] =
          (biased_slot == UINT32_MAX || slot == biased_slot ? bias : 0) + i;
    auto status = builder.publish_tensor(
        slot, Region::whole(schema.tensors[slot].sample_shape()),
        ByteView(reinterpret_cast<uint8_t*>(samples.data()),
                 samples.size() * 4));
    require(status.ok(), status.message);
  }
  return builder.seal().take_value();
}
void resource_admission() {
  std::vector<Region> boxes;
  for (uint64_t i = 0; i < 128; ++i)
    boxes.emplace_back(std::vector<RegionDimension>{{i, 1}});
  ResourceLimits limits;
  limits.capacity[ResourceKind::Host] = 512;
  limits.capacity[ResourceKind::Metadata] = 512;
  ResourceBudget small(limits);
  {
    ErrorCode failure = ErrorCode::Ok;
    ResourceAllocationScope scope(small, &failure);
    auto footprint = Footprint::from_regions({128}, boxes);
    require(!footprint.ok() &&
                footprint.status().code == ErrorCode::ResourceExhausted,
            "transient boxes admission");
    require(failure == ErrorCode::ResourceExhausted,
            "sticky metadata admission");
  }
  zero(small);
  ResourceBudget roomy;
  {
    ResourceAllocationScope scope(roomy);
    auto footprint = Footprint::from_regions({128}, boxes).take_value();
    require(footprint.boxes().size() == 1, "adjacent boxes normalize");
    require(roomy.statistics().peak[ResourceKind::Metadata] >
                roomy.statistics().live[ResourceKind::Metadata],
            "transient peak accounted");
  }
  zero(roomy);
  auto schema = unified_result_fixture::image_schema();
  schema.tensors.resize(16, schema.tensors[0]);
  for (unsigned i = 0; i < schema.tensors.size(); ++i)
    schema.tensors[i].key = std::string(120, 'x') + std::to_string(i);
  limits.capacity[ResourceKind::Host] = 1024;
  limits.capacity[ResourceKind::Metadata] = 1024;
  ResourceBudget low(limits);
  auto refused = ResultBuilder::start(low, schema, "large");
  require(refused.status().code == ErrorCode::ResourceExhausted,
          "large typed image metadata refused");
  zero(low);
  // Two new frame pages cannot be partially committed when the second fails.
  limits = ResourceLimits{};
  limits.capacity[ResourceKind::Payload] = 32768;
  ResourceBudget pages(limits);
  {
    schema = unified_result_fixture::image_schema();
    schema.tensors[0].batch_axes[0] = 3;
    schema.tensors[0].batch_axes[1] = 1;
    schema.tensors[0].descriptor.shape = {1, 1};
    auto builder =
        ResultBuilder::start(pages, schema, "transaction", {}, 1).take_value();

    float value = 4;
    require(builder
                .publish_tensor(0, Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}}),
                                ByteView(reinterpret_cast<uint8_t*>(&value), 4))
                .ok(),
            "old frame");
    auto old = builder.reference().capture().take_value();
    const auto baseline = pages.statistics().live[ResourceKind::Payload];
    ResourceCapacity occupied;
    occupied[ResourceKind::Payload] = 32768 - 2 * baseline;
    auto blocker = pages.reserve(occupied).take_value();
    float next[] = {5, 6};
    auto failed =
        builder.publish_tensor(0, Region({{1, 2}, {0, 1}, {0, 1}, {0, 1}}),
                               ByteView(reinterpret_cast<uint8_t*>(next), 8));
    require(failed.code == ErrorCode::ResourceExhausted,
            "second frame preparation refused");
    require(pages.statistics().live[ResourceKind::Payload] == 32768 - baseline,
            "uncommitted frame pages retired");
    require(builder.reference().descriptor(false).value().revision() ==
                old.descriptor(false).value().revision(),
            "failed transaction does not advance revision");
    float read = 0;
    require(old.read_tensor(old.descriptor(false).value(), 0, {0, 0, 0, 0},
                            &read, 4)
                    .ok() &&
                read == 4,
            "old captured frame survives failure");
  }
  zero(pages);
}
struct Join {
  unsigned stage = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto samples =
        phase.query.tensor_outputs
            ? Result<Footprint>(*phase.query.tensor_outputs)
            : Footprint::all(
                  phase.query.output.result_schema->tensors[0].sample_shape());
    if (!samples.ok())
      return Result<ResultProgramPoll>(samples.status());
    const bool temporal = phase.query.parameters.count("temporal") &&
                          std::get<bool>(phase.query.parameters.at("temporal"));
    auto upstream = samples;
    if (temporal) {
      auto admitted = phase.resources.reserve(ResourceCapacity::host(
          samples.value().boxes().size() *
              (sizeof(Region) + 5 * sizeof(RegionDimension)),
          samples.value().boxes().size() *
              (sizeof(Region) + 5 * sizeof(RegionDimension))));
      if (!admitted.ok())
        return Result<ResultProgramPoll>(admitted.status());
      std::vector<Region> boxes;
      boxes.reserve(samples.value().boxes().size());
      for (const auto& box : samples.value().boxes()) {
        auto dimensions = box.dimensions();
        if (dimensions[0].extent != samples.value().shape()[0])
          dimensions[0].offset =
              (dimensions[0].offset + 1) % samples.value().shape()[0];
        boxes.emplace_back(std::move(dimensions));
      }
      upstream = Footprint::from_regions(samples.value().shape(), boxes);
      if (!upstream.ok())
        return Result<ResultProgramPoll>(upstream.status());
    }
    if (!stage++) {
      ResultProgramNeed need;
      need.tiles = {take(sdk::tile_need_for_samples(
                        phase.resources, phase.query, 0, 0, upstream.value())),
                    take(sdk::tile_need_for_samples(
                        phase.resources, phase.query, 1, 0, samples.value()))};
      return Result<ResultProgramPoll>(std::move(need));
    }
    auto builder = ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent);
    if (!builder.ok())
      return Result<ResultProgramPoll>(builder.status());
    auto output = builder.take_value();
    Status status;
    for (const auto& box : samples.value().boxes()) {
      auto count = box.element_count().value();
      auto bytes = phase.allocator.allocate(count * 4);
      if (!bytes.ok())
        return Result<ResultProgramPoll>(bytes.status());
      auto buffer = bytes.take_value();
      uint64_t position = 0;
      auto selected = Footprint::from_regions(samples.value().shape(), {box});
      if (!selected.ok())
        return Result<ResultProgramPoll>(selected.status());
      status = selected.value().visit(
          [&](const auto& at) {
            float first = 0, second = 0;
            auto source = at;
            if (temporal)
              source[0] = (source[0] + 1) % samples.value().shape()[0];
            auto read = phase.read_tensor(0, 0, source, &first, 4);
            if (!read.ok())
              return read;
            read = phase.read_tensor(1, 0, at, &second, 4);
            if (!read.ok())
              return read;
            const float sum = first + second;
            std::memcpy(buffer.data() + 4 * position++, &sum, 4);
            return Status::success();
          },
          count);
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
      status =
          output.publish_tensor(0, box, ByteView(buffer.data(), buffer.size()));
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
    }
    auto sealed = output.seal();
    return sealed.ok() ? Result<ResultProgramPoll>(
                             ResultPublication{sealed.take_value(), true})
                       : Result<ResultProgramPoll>(sealed.status());
  }
};
Status join(OperationRegistry* registry, const SchemaTemplate& schema) {
  OperationDefinition op;
  op.key = "test.image.join";
  op.traits = unified_result_fixture::traits(2, sizeof(Join));
  op.traits.parameter_schema = {
      {"temporal", OperationParameterType::Bool, false, false, 0, 0}};
  unified_result_fixture::result_output(&op.traits.outputs[0], schema);
  for (auto& port : op.traits.input_schema)
    unified_result_fixture::result_port(&port, schema);
  op.map_dependencies =
      [](const ResultStaticQuery& query) -> Result<ResultStaticMapping> {
    const bool temporal = query.parameters.count("temporal") &&
                          std::get<bool>(query.parameters.at("temporal"));
    TileIndex first = query.tile_unit;
    if (temporal) {
      const auto& tensor = query.metadata.inputs[0].result_schema->tensors[0];
      const auto count = query.metadata.input_tile_grids[0][0].tile_count();
      const auto per_frame = count / tensor.batch_axes[0];
      first = (first + per_frame) % count;
    }
    ResultStaticMapping mapping;
    for (uint32_t input = 0; input < 2; ++input) {
      const auto tile = input == 0 ? first : query.tile_unit;
      auto tiles = TileSet::range(ResourceBudget{}, tile, tile + 1);
      if (!tiles.ok())
        return Result<ResultStaticMapping>(tiles.status());
      mapping.inputs.push_back({input, 0, false, tiles.take_value()});
    }
    return Result<ResultStaticMapping>(std::move(mapping));
  };
  op.start_result = [](const ResultProgramQuery&,
                       const BufferAllocator& allocator) {
    return ResultContinuation::make<Join>(allocator);
  };
  return registry->register_operation(std::move(op));
}
ResourceVector<TileUnit> dirty(const ExecutionDependencies& dependencies,
                               const ResourceBudget& root, TileIndex tile) {
  ResourceVector<TileChange> changes{ResourceAllocator<TileChange>(root)};
  changes.push_back({PlanWorkflowInput{0}, 0, 0,
                     take(TileSet::from_indices(root, {tile})), false});
  return take(dependencies.dirty_units(changes));
}
TileRequest request(std::initializer_list<TileIndex> tiles) {
  return {{{0, take(TileSet::from_indices(ResourceBudget{}, tiles))}}};
}
void alias_diamond() {
  auto schema = unified_result_fixture::image_schema();
  auto registry = std::make_shared<OperationRegistry>();
  require(join(registry.get(), schema).ok(), "join registration");
  require(registry->freeze().ok(), "freeze joins");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = context.resource_budget().take_value();
  ExecutionBinding binding;
  binding.name = "source";
  binding.result = image(root, schema);
  WorkflowDocument doc;
  WorkflowInputDeclaration declaration;
  declaration.id = 1;
  declaration.name = "source";
  declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
  doc.inputs = {declaration};
  doc.nodes = {
      {1,
       "test.image.join",
       {WorkflowInputReference{1}, WorkflowInputReference{1}},
       {}},
      {2,
       "test.image.join",
       {WorkflowInputReference{1}, WorkflowInputReference{1}},
       {}},
      {3,
       "test.image.join",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{1, "value"}},
       {}},
      {4,
       "test.image.join",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{2, "value"}},
       {}}};
  doc.outputs = {{"left", 3, "value"}, {"right", 4, "value"}};
  GraphContext graph(doc);
  PlanningOptions geometry;
  geometry.tile_extent = 2;
  auto plan =
      Compiler(registry).compile(graph, geometry).take_value().plan.value();
  auto frozen = context.freeze(plan, {{binding}}).take_value();
  const auto q = request({7});
  for (unsigned iteration = 0; iteration < 2; ++iteration) {
    auto result =
        context.execute_fragments(frozen, {{"left", q}, {"right", q}});
    require(result.ok(), result.status().message);
    ResourceVector<TileChange> changes{ResourceAllocator<TileChange>(root)};
    changes.push_back({PlanWorkflowInput{0}, 0, 0, q.members.at(0), false});
    {
      const auto available = root.available_capacity();
      const auto bytes = std::min(available[ResourceKind::Host],
                                  available[ResourceKind::Metadata]) -
                         ResourceBudget::lease_metadata_bytes();
      auto blocker = take(root.reserve(ResourceCapacity::host(bytes, bytes)));
      auto denied = result.value().dependencies.dirty_units(changes);
      require(
          !denied.ok() && denied.status().code == ErrorCode::ResourceExhausted,
          "dirty closure metadata admission");
    }
    const auto before_maps = root.statistics().live[ResourceKind::Metadata];
    {
      auto units = dirty(result.value().dependencies, root, 7);
      require(units.size() == 6, "diamond includes calls and named returns");
      for (std::size_t step = 0; step < 4; ++step)
        require(units[step] == TileUnit{step, 0, 0, 7},
                "diamond unit identity");
      require(
          units[4] == TileUnit{plan.output("left").record_index, 0, 0, 7} &&
              units[5] == TileUnit{plan.output("right").record_index, 0, 0, 7},
          "diamond return unit identity");
      require(root.statistics().live[ResourceKind::Metadata] > before_maps,
              "returned dirty units own root capacity");
    }
    require(root.statistics().live[ResourceKind::Metadata] == before_maps,
            "dirty unit capacity retires");
    for (const auto& key : {"left", "right"}) {
      auto owner = result.value().results.at(key);
      float read = 0;
      require(owner.read_tensor(owner.descriptor().value(), 0, {1, 1, 1, 2},
                                &read, 4)
                      .ok() &&
                  read == 120,
              "nonzero origin/frame/layer diamond result");
    }
  }

  doc.nodes.resize(1);
  doc.nodes[0].parameters = {{"temporal", true}};
  doc.outputs = {{"out", 1, "value"}};
  GraphContext temporal_graph(doc);
  auto temporal_plan = Compiler(registry)
                           .compile(temporal_graph, geometry)
                           .take_value()
                           .plan.value();
  auto result = context.execute_fragments(
      take(context.freeze(temporal_plan, {{binding}})),
      {{"out", request({1})}});
  require(result.ok(), result.status().message);
  require(dirty(result.value().dependencies, root, 5) ==
              ResourceVector<TileUnit>(
                  {TileUnit{0, 0, 0, 1},
                   TileUnit{temporal_plan.output("out").record_index, 0, 0, 1}},
                  ResourceAllocator<TileUnit>(root)),
          "cross-frame tile dependency");
  require(dirty(result.value().dependencies, root, 4).empty(),
          "unrequested neighboring tile stays clean");
  // Width coordinates 2 and 3 are in the same square tile and dirty together.
  auto grid = take(TileGrid::from_tensor(schema.tensors[0], 2));
  const uint64_t at[] = {1, 0, 1, 3};
  require(take(sdk::tile_for_coordinate(grid, at, 4)) == 5 &&
              dirty(result.value().dependencies, root, 5).size() == 2,
          "same-tile edit dirties consumer");
}
SchemaTemplate chunk_schema(uint64_t count, ElementType type, const char* id) {
  SchemaTemplate schema;
  schema.id = id;
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {type, count == 1 ? std::vector<uint64_t>{1}
                                        : std::vector<uint64_t>{1, count}};
  member.layout.spatial = count != 1;
  member.layout.channel_axis.reset();
  schema.tensors.push_back(std::move(member));
  return schema;
}
struct Probe {
  bool requested = false, bad;
  explicit Probe(bool bad) : bad(bad) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!requested) {
      requested = true;
      auto acquisition = sdk::to_whole(phase, 0);
      if (!acquisition.ok())
        return Result<ResultProgramPoll>(acquisition.status());
      auto acquired = acquisition.take_value();
      if (auto* need = std::get_if<ResultProgramNeed>(&acquired))
        return Result<ResultProgramPoll>(std::move(*need));
    }
    float value = 0;
    if (bad)
      static_cast<void>(phase.read_tensor(0, 1, {1, 0, 0, 0}, &value, 4));
    check(phase.read_tensor(0, 1, {0, 1, 0, 2}, &value, 4));
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    check(builder.publish_tensor(
        0, Region::whole({1}), {reinterpret_cast<const uint8_t*>(&value), 4}));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
struct Escaped {
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto acquisition = sdk::to_whole(phase, 0);
    if (!acquisition.ok())
      return Result<ResultProgramPoll>(acquisition.status());
    return Result<ResultProgramPoll>(
        std::get<ResultProgramNeed>(acquisition.take_value()));
  }
};
void all_member_acquisition() {
  auto schema = unified_result_fixture::image_schema();
  auto depth = schema.tensors[0];
  depth.key = "depth";
  depth.batch_axes = {1, 2};
  depth.descriptor.shape = {1, 3};
  schema.tensors.push_back(depth);
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition op;
  op.key = "test.image.probe";
  op.traits = unified_result_fixture::traits(1, sizeof(Probe));
  unified_result_fixture::result_port(&op.traits.input_schema[0], schema);
  unified_result_fixture::result_output(
      &op.traits.outputs[0],
      chunk_schema(1, ElementType::Float32, "test.probe.result"));
  op.traits.parameter_schema = {
      {"bad", OperationParameterType::Bool, true, false, 0, 0}};
  op.start_result = [](const auto& query, const auto& allocator) {
    return ResultContinuation::make<Probe>(
        allocator, std::get<bool>(query.parameters.at("bad")));
  };
  check(registry->register_operation(op));
  op.key = "test.image.escaped";
  op.traits.outputs[0].input_indices = std::vector<uint32_t>{};
  op.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<Escaped>(allocator);
  };
  check(registry->register_operation(op));
  check(registry->freeze());
  ExecutionContext context(registry);
  auto root = take(context.resource_budget());
  ExecutionBinding binding{"source", image(root, schema)};
  WorkflowDocument doc;
  doc.inputs = {multi_result::declaration(1, "source", schema)};
  doc.nodes = {
      {1, "test.image.probe", {WorkflowInputReference{1}}, {{"bad", false}}}};
  doc.outputs = {{"out", 1, "value"}};
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(
                       graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1}))
                  .plan.value();
  auto handle = take(context.open_demand(plan, {{binding}}));
  const DemandQuery query{{"out", request({0})}};
  auto result = take(handle.request(query));
  auto record = take(result.dependencies.tile_record({0, 0, 0}));
  require(record && record->inputs.size() == 2,
          "explicit acquisition records both tensor members");
  for (unsigned slot = 0; slot < 2; ++slot) {
    const auto grid = take(TileGrid::from_tensor(schema.tensors[slot], 1));
    require(record->inputs[slot].tensor_slot == slot &&
                record->inputs[slot].tiles ==
                    take(TileSet::range(root, 0, grid.tile_count())),
            "member acquisition covers its complete local grid");
  }
  float value = 0;
  check(result.results.at("out").read_tensor(
      take(result.results.at("out").descriptor()), 0, {0}, &value, 4));
  require(value == 5, "second tensor member value");
  for (auto slot : {0u, 1u}) {
    binding.result = image(root, schema, slot ? 100 : 50, slot);
    auto update = take(handle.replace_bindings({{binding}}));
    require(update.potential_dirty.at("out").members.at(0) ==
                take(TileSet::from_indices(root, {0})),
            "any acquired member edit dirties consumer");
    auto changed = take(handle.request(query));
    check(changed.results.at("out").read_tensor(
        take(changed.results.at("out").descriptor()), 0, {0}, &value, 4));
    require(value == (slot ? 105 : 5), "member replacement numerical value");
  }
  auto empty = take(handle.request({{"out", TileRequest{}}}));
  require(
      !take(empty.dependencies.tile_record({0, 0, 0})) &&
          take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
      "empty request has no input dependency or payload");
  for (bool escaped : {false, true}) {
    auto invalid = doc;
    invalid.nodes[0].operation =
        escaped ? "test.image.escaped" : "test.image.probe";
    invalid.nodes[0].parameters["bad"] = true;
    GraphContext bad_graph(invalid);
    auto bad_plan = take(Compiler(registry).compile(bad_graph)).plan.value();
    auto bad = context.execute(bad_plan, {{binding}});
    require(!bad.ok() && bad.status().code == ErrorCode::InvalidArgument,
            "projection escape or ignored out-of-domain read fails");
  }
}

struct ChunkObserver {
  std::vector<WeakResultRef> produced;
  uint64_t maximum_live = 0;
  bool keep_first = false;
  uint64_t fail_at = UINT64_MAX;
};
Result<ResultStaticMapping> input_interval(TileIndex begin, TileIndex end) {
  auto tiles = TileSet::range(ResourceBudget{}, begin, end);
  if (!tiles.ok())
    return Result<ResultStaticMapping>(tiles.status());
  ResultStaticMapping mapping;
  mapping.inputs.push_back({0, 0, false, tiles.take_value()});
  return Result<ResultStaticMapping>(std::move(mapping));
}
Result<ResultStaticMapping> copy_mapping(const ResultStaticQuery& query) {
  return input_interval(query.tile_unit, query.tile_unit + 1);
}
Result<ResultStaticMapping> prefix_mapping(const ResultStaticQuery& query) {
  return input_interval(0, query.tile_unit + 1);
}
struct ChunkCopy {
  unsigned stage = 0;
  std::shared_ptr<ChunkObserver> observer;
  explicit ChunkCopy(std::shared_ptr<ChunkObserver> value)
      : observer(std::move(value)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto& samples = *phase.query.tensor_outputs;
    if (!stage++) {
      ResultProgramNeed need;
      need.tiles = {take(sdk::tile_need_for_samples(
          phase.resources, phase.query, 0, 0, samples))};
      return Result<ResultProgramPoll>(std::move(need));
    }
    const auto region = samples.boxes()[0];
    const auto at = region.dimensions().back().offset;
    if (at == observer->fail_at)
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed, "deliberate chunk failure"});
    auto builder = ResultBuilder::start(
                       phase.resources, *phase.query.output.result_schema,
                       phase.query.semantic_key, {}, phase.query.tile_extent)
                       .take_value();
    Status status;
    double value = 0;
    status = phase.read_tensor(0, 0, {0, at}, &value, 8);
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    auto bytes = take(phase.allocator.allocate(8));
    std::memcpy(bytes.data(), &value, 8);
    status =
        builder.publish_tensor(0, region, StridedLayout{0, {8, 8}, {0, at}},
                               std::move(bytes).freeze());
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    auto result = builder.seal().take_value();
    observer->produced.push_back(result.weak());
    return Result<ResultProgramPoll>(
        ResultPublication{std::move(result), true});
  }
};
struct ChunkSum {
  uint64_t cursor = 0;
  double sum = 0;
  bool waiting = false;
  ResultTensorReadWindow retained;
  std::shared_ptr<ChunkObserver> observer;
  explicit ChunkSum(std::shared_ptr<ChunkObserver> value)
      : observer(std::move(value)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto count =
        phase.query.inputs[0].result_schema->tensors[0].sample_count().value();
    if (waiting) {
      double value = 0;
      auto status = phase.read_tensor(0, 0, {0, cursor}, &value, 8);
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
      if (!cursor && observer->keep_first) {
        auto window =
            phase.tensors->at({0, 0}).acquire(Region({{0, 1}, {0, 1}}));
        if (!window.ok())
          return Result<ResultProgramPoll>(window.status());
        retained = window.take_value();
      }
      if (retained.valid()) {
        auto first = retained.row_run({0, 0});
        if (!first.ok())
          return Result<ResultProgramPoll>(first.status());
        double first_value = 0;
        std::memcpy(&first_value, first.value().data, 8);
        require(first_value == 1,
                "explicit window retains old chunk bytes across Need polls");
      }
      uint64_t live = 0;
      for (const auto& weak : observer->produced)
        if (weak.lock().valid())
          ++live;
      observer->maximum_live = std::max(observer->maximum_live, live);
      require(live == count,
              "all static input tiles remain ready until consuming closure "
              "retires");
      sum += value;
      ++cursor;
      waiting = false;
    }
    if (cursor < count) {
      auto samples =
          Footprint::from_regions({1, count}, {Region({{0, 1}, {cursor, 1}})});
      if (!samples.ok())
        return Result<ResultProgramPoll>(samples.status());
      ResultProgramNeed need;
      need.tiles = {take(sdk::tile_need_for_samples(
          phase.resources, phase.query, 0, 0, samples.value()))};
      waiting = true;
      return Result<ResultProgramPoll>(std::move(need));
    }
    auto builder =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key, {})
            .take_value();
    Status status;
    status = builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const uint8_t*>(&sum), 8));
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    return Result<ResultProgramPoll>(
        ResultPublication{builder.seal().take_value(), true});
  }
};
void staged_input_retirement() {
  for (const auto mode : {0U, 1U, 2U}) {
    const uint64_t count = mode == 0 ? 1030 : 32;
    auto observer = std::make_shared<ChunkObserver>();
    observer->keep_first = mode == 1;
    if (mode == 2)
      observer->fail_at = 17;
    auto schema =
        chunk_schema(count, ElementType::Float64, "test.chunk.samples");
    auto output_schema =
        chunk_schema(1, ElementType::Float64, "test.chunk.sum");
    auto registry = std::make_shared<OperationRegistry>();
    OperationDefinition copy;
    copy.key = "test.chunk.copy";
    copy.traits = unified_result_fixture::traits(1, sizeof(ChunkCopy));
    unified_result_fixture::result_port(&copy.traits.input_schema[0], schema);
    unified_result_fixture::result_output(&copy.traits.outputs[0], schema);
    copy.map_dependencies = copy_mapping;
    copy.start_result = [observer](const auto&, const auto& allocator) {
      return ResultContinuation::make<ChunkCopy>(allocator, observer);
    };
    require(registry->register_operation(std::move(copy)).ok(),
            "chunk copy registration");
    OperationDefinition reduction;
    reduction.key = "test.chunk.sum";
    reduction.traits = unified_result_fixture::traits(1, sizeof(ChunkSum));
    reduction.traits.outputs[0].maximum_dependency_stages = 4096;
    unified_result_fixture::result_port(&reduction.traits.input_schema[0],
                                        schema);
    unified_result_fixture::result_output(&reduction.traits.outputs[0],
                                          output_schema);
    reduction.start_result = [observer](const auto&, const auto& allocator) {
      return ResultContinuation::make<ChunkSum>(allocator, observer);
    };
    require(registry->register_operation(std::move(reduction)).ok() &&
                registry->freeze().ok(),
            "chunk sum registration and freeze");
    ExecutionContextConfig config;
    config.maximum_live_bytes = count * 8 + 4096;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Payload] =
        count * 8 + 4096;
    config.managed_resources->capacity[ResourceKind::Metadata] = 64 * 1048576;
    auto context = std::make_unique<ExecutionContext>(registry, config);
    auto root = context->resource_budget().take_value();
    {
      auto buffer = root.allocator().allocate(8).take_value();
      const double one = 1;
      std::memcpy(buffer.data(), &one, 8);
      auto builder =
          ResultBuilder::start(root, schema, "chunk.input").take_value();

      require(builder
                  .publish_tensor(0, Region::whole({1, count}), {0, {0, 0}},
                                  std::move(buffer).freeze())
                  .ok(),
              "broadcast chunk source");
      ExecutionBinding binding;
      binding.name = "source";
      binding.result = builder.seal().take_value();
      WorkflowDocument doc;
      WorkflowInputDeclaration declaration;
      declaration.id = 1;
      declaration.name = "source";
      declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
      doc.inputs = {declaration};
      doc.nodes = {{1, "test.chunk.copy", {WorkflowInputReference{1}}, {}},
                   {2, "test.chunk.sum", {WorkflowNodeOutput{1, "value"}}, {}}};
      doc.outputs = {{"out", 2, "value"}};
      GraphContext graph(doc);
      auto plan =
          Compiler(registry)
              .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
              .take_value()
              .plan.value();
      ExecutionOptions options;
      options.maximum_dependency_work = UINT64_C(1) << 30;
      options.dependencies.maximum_work = UINT64_C(1) << 30;
      options.dependencies.sets.maximum_work = UINT64_C(1) << 30;
      auto result = context->execute(plan, {{binding}}, {}, options);
      if (mode == 2) {
        require(!result.ok() &&
                    result.status().code == ErrorCode::OperationFailed &&
                    result.status().detail.node_id == 1,
                "failed chunk preserves producer provenance");
      } else {
        require(result.ok(), result.status().message);
        auto output = result.value().results.at("out");
        double sum = 0;
        require(output.read_tensor(output.descriptor().take_value(), 0, {0},
                                   &sum, 8)
                        .ok() &&
                    sum == count,
                "public workflow completes chunk sum");
        for (uint64_t edited : {uint64_t{0}, count / 2, count - 1}) {
          auto units = dirty(result.value().dependencies, root, edited);
          require(
              units.size() == 3 && units[0] == TileUnit{0, 0, 0, edited} &&
                  units[1] == TileUnit{1, 0, 0} &&
                  units[2] == TileUnit{plan.output("out").record_index, 0, 0},
              "historical tile dependency survives payload retirement");
        }
        auto record = take(result.value().dependencies.tile_record({1, 0, 0}));
        require(record && record->inputs.size() == 1 &&
                    record->inputs[0].tiles.size() == count,
                "sum records all source tiles");
        require(observer->maximum_live == count,
                "input tiles live until consumer closure retires");
      }
      for (const auto& weak : observer->produced)
        require(!weak.lock().valid(),
                "chunk payload owners retire when execution actors finish");
      require(root.statistics().peak[ResourceKind::Payload] <= count * 8 + 4096,
              "retained tiles remain within explicit payload capacity");
    }
    context.reset();
    zero(root);
  }
}
struct BlockObserver {
  unsigned mode = 0, computations = 0;
  ResultRef foreign;
  WeakResultRef previous;
  ResultRef previous_owner;
  CancellationSource cancellation;
};
struct BlockProgram {
  unsigned stage = 0;
  std::shared_ptr<BlockObserver> observer;
  explicit BlockProgram(std::shared_ptr<BlockObserver> value)
      : observer(std::move(value)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    if (!stage++) {
      ResultProgramNeed need;
      need.tiles = {
          {0, 0, false, take(TileSet::from_indices(phase.resources, {0}))}};
      return Answer(std::move(need));
    }
    const auto state = [&](double number) {
      auto schema = chunk_schema(1, ElementType::Float64, "test.block.state");
      if (observer->mode == 10)
        schema.tensors[0].descriptor.shape = {1, 4};
      auto builder =
          ResultBuilder::start(phase.resources, schema, "block.state", {}, 128)
              .take_value();

      const double colors[] = {number / 10, 0, 0, 0};
      require(
          builder
              .publish_tensor(
                  0, Region::whole(schema.tensors[0].sample_shape()),
                  observer->mode == 10
                      ? ByteView(reinterpret_cast<const uint8_t*>(colors), 32)
                      : ByteView(reinterpret_cast<const uint8_t*>(&number), 8))
              .ok(),
          "block state payload");
      return builder.seal().take_value();
    };
    auto incoming = state(1);
    if (observer->mode == 11)
      incoming = incoming.capture().take_value();
    auto prepared = state(3);
    ResourceLease blocker;
    if (observer->mode == 7) {
      auto available = phase.resources.available_capacity();
      auto bytes = std::min(available[ResourceKind::Host],
                            available[ResourceKind::Metadata]) -
                   ResourceBudget::lease_metadata_bytes();
      blocker = phase.resources.reserve(ResourceCapacity::host(bytes, bytes))
                    .take_value();
    }
    if (observer->mode == 11) {
      // Vary only optional headroom after all mandatory state is prepared.
      // This covers canonical-key and input-read window admissions separately.
      for (uint64_t headroom = 256; headroom <= 8192; headroom += 16) {
        const auto available = phase.resources.available_capacity();
        const auto bytes = std::min(available[ResourceKind::Host],
                                    available[ResourceKind::Metadata]) -
                           ResourceBudget::lease_metadata_bytes() - headroom;
        auto quota =
            phase.resources.reserve(ResourceCapacity::host(bytes, bytes))
                .take_value();
        auto trial = phase.block(1, 0, 1, headroom, incoming,
                                 [&] { return Result<ResultRef>(prepared); });
        require(trial.ok(),
                "optional block key and authorized hash read admission");
      }
    }
    auto result = phase.block(
        1, 0, observer->mode == 5 ? 0 : 1, 1,
        observer->mode == 6 ? observer->foreign : incoming,
        [&]() -> Result<ResultRef> {
          ++observer->computations;
          if (observer->mode == 2)
            throw std::runtime_error("block compute failure");
          if (observer->mode == 3) {
            double ignored = 0;
            static_cast<void>(phase.read_tensor(0, 0, {0, 1}, &ignored, 8));
          }
          if (observer->mode == 4)
            observer->cancellation.cancel();
          if (observer->mode == 1)
            return Result<ResultRef>(observer->foreign);
          if (observer->mode == 8) {
            auto available = phase.resources.available_capacity();
            auto bytes = std::min(available[ResourceKind::Host],
                                  available[ResourceKind::Metadata]) -
                         ResourceBudget::lease_metadata_bytes();
            blocker =
                phase.resources.reserve(ResourceCapacity::host(bytes, bytes))
                    .take_value();
            return Result<ResultRef>(prepared);
          }
          if (observer->mode == 7)
            return Result<ResultRef>(prepared);
          double value = 0;
          auto status = phase.read_tensor(0, 0, {0, 0}, &value, 8);
          return status.ok() ? Result<ResultRef>(state(1 + value))
                             : Result<ResultRef>(status);
        });
    blocker = {};
    double number = 3;
    if (result.ok()) {
      auto facts = result.value().descriptor().take_value();
      require(
          result.value()
              .read_tensor(facts, 0,
                           observer->mode == 10 ? std::vector<uint64_t>{0, 0}
                                                : std::vector<uint64_t>{0},
                           &number, 8)
              .ok(),
          "Result block read");
      if (observer->previous.lock().valid())
        require(
            observer->previous.lock().object_id() != result.value().object_id(),
            "block hit creates independent current state");
      observer->previous = result.value().weak();
      observer->previous_owner = result.value();
    }
    // Ignore any service error deliberately; the enclosing publication must
    // still report its original sticky cause.
    auto builder = ResultBuilder::start(
                       phase.resources, *phase.query.output.result_schema,
                       phase.query.semantic_key, {}, phase.query.tile_extent)
                       .take_value();

    require(builder
                .publish_tensor(
                    0, phase.query.tensor_outputs->boxes()[0],
                    ByteView(reinterpret_cast<const uint8_t*>(&number), 8))
                .ok(),
            "block output payload");
    return Answer(ResultPublication{builder.seal().take_value(), true});
  }
};
void result_blocks() {
  for (unsigned mode = 0; mode < 13; ++mode) {
    auto observer = std::make_shared<BlockObserver>();
    observer->mode = mode;
    auto schema = chunk_schema(2, ElementType::Float64, "test.block.input");
    if (mode == 12)
      schema.id = std::string(128, 'x');
    auto registry = std::make_shared<OperationRegistry>();
    OperationDefinition operation;
    operation.key = "test.block";
    operation.traits = unified_result_fixture::traits(1, sizeof(BlockProgram));
    if (mode == 9)
      operation.traits.workspace_bytes = 0;
    unified_result_fixture::result_port(&operation.traits.input_schema[0],
                                        schema);
    unified_result_fixture::result_output(&operation.traits.outputs[0], schema);
    operation.map_dependencies = [](const ResultStaticQuery&) {
      return input_interval(0, 1);
    };
    operation.start_result = [observer](const auto&, const auto& allocator) {
      return ResultContinuation::make<BlockProgram>(allocator, observer);
    };
    auto registered = registry->register_operation(std::move(operation));
    require(registered.ok(), "block registration mode=" + std::to_string(mode) +
                                 " message=" + registered.message);
    require(registry->freeze().ok(), "block registry freeze");
    ExecutionContextConfig config;
    config.result_cache_bytes = 4096;
    config.maximum_dependency_cache_metadata = 1;
    config.managed_resources = ResourceLimits{};
    auto context = std::make_unique<ExecutionContext>(registry, config);
    auto root = context->resource_budget().take_value();
    ResourceBudget foreign;
    {
      auto make_source = [&](const ResourceBudget& budget) {
        auto builder =
            ResultBuilder::start(budget, schema, "block.input").take_value();

        double numbers[] = {2, 4};
        require(builder
                    .publish_tensor(
                        0, Region::whole({1, 2}),
                        ByteView(reinterpret_cast<const uint8_t*>(numbers), 16))
                    .ok(),
                "block source payload");
        return builder.seal().take_value();
      };
      auto input = make_source(root);
      observer->foreign = make_source(foreign);
      WorkflowDocument document;
      WorkflowInputDeclaration declaration;
      declaration.id = 1;
      declaration.name = "x";
      declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
      document.inputs = {declaration};
      WorkflowNode node;
      node.id = 2;
      node.operation = "test.block";
      node.inputs = {WorkflowInputReference{1}};
      document.nodes = {node};
      document.outputs = {{"out", 2, "value"}};
      GraphContext graph(document);
      auto plan =
          Compiler(registry)
              .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
              .take_value()
              .plan.value();
      auto frozen = context->freeze(plan, {{{"x", input}}}).take_value();
      auto point = [&](uint64_t at) { return request({at}); };
      ExecutionOptions options;
      if (mode == 12)
        options.maximum_dependency_cache_work = 16;
      auto first = context->execute_fragments(
          frozen, {{"out", point(0)}}, observer->cancellation.token(), options);
      if (mode == 0 || mode >= 7) {
        require(first.ok(),
                "valid Result block execution mode=" + std::to_string(mode) +
                    " code=" +
                    std::to_string(static_cast<unsigned>(first.status().code)) +
                    " message=" + first.status().message);
        if (mode == 0 || mode == 9) {
          auto second = context->execute_fragments(frozen, {{"out", point(1)}});
          require(second.ok() && observer->computations == 1,
                  "completed block reuse excludes requested output footprint");
          observer->previous_owner = {};
          require(!observer->previous.lock().valid(),
                  "block cache retains no prior Result state owner");
          auto record =
              take(second.value().dependencies.tile_record({0, 0, 0, 1}));
          require(record && record->inputs.size() == 1 &&
                      record->inputs[0].tiles == request({0}).members.at(0),
                  "block hit keeps current tile Need");
        } else if (mode == 12) {
          require(
              observer->computations == 1 &&
                  first.value().diagnostics.dependency_cache_work <= 16,
              "large schema identity respect finite optional metadata fuel");
        } else if (mode == 11) {
          require(observer->computations == 1,
                  "optional metadata read refusals do not poison later "
                  "mandatory computation");
        } else if (mode == 10) {
          auto second = context->execute_fragments(frozen, {{"out", point(1)}});
          require(second.ok() && observer->computations == 1,
                  "multi-element block states reuse completed data");
        } else {
          require(
              observer->computations == 1 &&
                  context->cache_statistics().entries == 0,
              "optional allocation refusal preserves successful computation");
        }
      } else {
        const auto expected = mode == 2   ? ErrorCode::OperationFailed
                              : mode == 4 ? ErrorCode::Cancelled
                                          : ErrorCode::InvalidArgument;
        require(!first.ok() && first.status().code == expected,
                "ignored Result block failure remains sticky");
      }
      observer->foreign = {};
      observer->previous_owner = {};
    }
    context.reset();
    zero(root);
    zero(foreign);
  }
}

struct CheckpointObserver {
  std::vector<uint64_t> starts, restored;
  std::vector<uint64_t> targets;
  ResultCheckpoint retained;
  ResultRef foreign;
  bool bad_read = false, bad_state = false, block_retention = false;
  CancellationSource* cancel = nullptr;
};
struct CheckpointSum {
  std::shared_ptr<CheckpointObserver> observer;
  uint64_t cursor = 0, start = 0;
  double carry = 0;
  bool started = false, waiting = false;
  explicit CheckpointSum(std::shared_ptr<CheckpointObserver> value)
      : observer(std::move(value)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    const auto& requested = *phase.query.tensor_outputs;
    const auto region = requested.boxes()[0];
    const auto target = region.dimensions().back().offset;
    const auto count = requested.shape().back();
    if (!started) {
      started = true;
      auto absent = phase.checkpoint_before(99, target);
      if (!absent.ok())
        return Answer(absent.status());
      require(!absent.value(), "checkpoint phase isolation");
      auto prior = phase.checkpoint_before(1, target);
      if (!prior.ok())
        return Answer(prior.status());
      if (prior.value()) {
        observer->retained = *prior.value();
        auto facts = prior.value()->state().descriptor();
        if (!facts.ok())
          return Answer(facts.status());
        auto status = prior.value()->state().read_tensor(facts.value(), 0, {0},
                                                         &carry, 8);
        if (!status.ok())
          return Answer(status);
        cursor = prior.value()->sequence() + 1;
        observer->restored.push_back(prior.value()->sequence());
        if (observer->bad_read) {
          double ignored = 0;
          static_cast<void>(
              phase.read_tensor(0, 0, {0, target + 1}, &ignored, 8));
        }
        if (observer->cancel) {
          observer->cancel->cancel();
          static_cast<void>(phase.checkpoint_publish(
              1, prior.value()->sequence(), prior.value()->state()));
        }
      }
      start = cursor;
      observer->starts.push_back(cursor);
      observer->targets.push_back(target);
    }
    if (waiting) {
      double value = 0;
      auto status = phase.read_tensor(0, 0, {0, cursor}, &value, 8);
      if (!status.ok())
        return Answer(status);
      carry += value;
      auto schema = chunk_schema(1, ElementType::Float64, "test.carry");
      auto builder =
          ResultBuilder::start(phase.resources, schema, "carry").take_value();
      status = builder.publish_tensor(
          0, Region::whole({1}),
          ByteView(reinterpret_cast<const uint8_t*>(&carry), 8));
      if (!status.ok())
        return Answer(status);
      auto state = builder.seal().take_value();
      if (observer->bad_state) {
        static_cast<void>(
            phase.checkpoint_publish(1, cursor, observer->foreign));
      } else {
        ResourceLease blocker;
        if (observer->block_retention) {
          const auto available = phase.resources.available_capacity();
          const auto bytes = std::min(available[ResourceKind::Host],
                                      available[ResourceKind::Metadata]) -
                             ResourceBudget::lease_metadata_bytes();
          blocker =
              phase.resources.reserve(ResourceCapacity::host(bytes, bytes))
                  .take_value();
        }
        status = phase.checkpoint_publish(1, cursor, state);
        if (!status.ok())
          return Answer(status);
      }
      ++cursor;
      waiting = false;
    }
    if (cursor <= target) {
      ResultProgramNeed need;
      auto samples =
          Footprint::from_regions({1, count}, {Region({{0, 1}, {cursor, 1}})})
              .take_value();
      need.tiles = {take(sdk::tile_need_for_samples(
          phase.resources, phase.query, 0, 0, samples))};
      waiting = true;
      return Answer(std::move(need));
    }
    auto builder = ResultBuilder::start(
                       phase.resources, *phase.query.output.result_schema,
                       phase.query.semantic_key, {}, phase.query.tile_extent)
                       .take_value();
    Status status;
    status = builder.publish_tensor(
        0, region, ByteView(reinterpret_cast<const uint8_t*>(&carry), 8));
    if (!status.ok())
      return Answer(status);
    return Answer(ResultPublication{builder.seal().take_value(), true});
  }
};
struct DynamicCheckpointSource {
  std::shared_ptr<std::atomic<unsigned>> calls;
  explicit DynamicCheckpointSource(
      std::shared_ptr<std::atomic<unsigned>> counter)
      : calls(std::move(counter)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto builder = ResultBuilder::start(
                       phase.resources, *phase.query.output.result_schema,
                       phase.query.semantic_key, {}, phase.query.tile_extent)
                       .take_value();
    Status status;
    const auto base = ++*calls;
    std::array<double, 8> values{};
    for (uint64_t i = 0; i < values.size(); ++i)
      values[i] = i + base;
    status = builder.publish_tensor(
        0, Region::whole({1, 8}),
        ByteView(reinterpret_cast<const uint8_t*>(values.data()), 64));
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    return Result<ResultProgramPoll>(
        ResultPublication{builder.seal().take_value(), true});
  }
};
void impure_shared_results() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    auto chunks = std::make_shared<ChunkObserver>();
    auto schema = chunk_schema(8, ElementType::Float64, "test.impure.shared");
    auto registry = std::make_shared<OperationRegistry>();
    OperationDefinition dynamic;
    dynamic.key = "test.impure.shared";
    dynamic.traits =
        unified_result_fixture::traits(0, sizeof(DynamicCheckpointSource));
    unified_result_fixture::result_output(&dynamic.traits.outputs[0], schema);
    dynamic.traits.deterministic = mode == 1;
    dynamic.traits.side_effect_free = mode == 0;
    dynamic.traits.cacheable = false;
    dynamic.start_result = [calls](const auto&, const auto& allocator) {
      return ResultContinuation::make<DynamicCheckpointSource>(allocator,
                                                               calls);
    };
    auto invalid = dynamic;
    invalid.key += ".cached";
    invalid.traits.cacheable = true;
    require(registry->register_operation(std::move(invalid)).code ==
                ErrorCode::InvalidArgument,
            "non-pure operations cannot declare cacheable outputs");
    require(registry
                ->register_operation(
                    sdk::with_whole_stage(std::move(dynamic), 1000000))
                .ok(),
            "non-pure shared-stage registration");
    OperationDefinition copy;
    copy.key = "test.impure.copy";
    copy.traits = unified_result_fixture::traits(1, sizeof(ChunkCopy));
    unified_result_fixture::result_port(&copy.traits.input_schema[0], schema);
    unified_result_fixture::result_output(&copy.traits.outputs[0], schema);
    copy.map_dependencies = copy_mapping;
    copy.start_result = [chunks](const auto&, const auto& allocator) {
      return ResultContinuation::make<ChunkCopy>(allocator, chunks);
    };
    require(registry->register_operation(std::move(copy)).ok() &&
                registry->freeze().ok(),
            "pure consumer of non-pure shared Result registration");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    auto context = std::make_unique<ExecutionContext>(registry, config);
    auto root = context->resource_budget().take_value();
    {
      WorkflowDocument doc;
      doc.nodes = {
          {1, "test.impure.shared", {}, {}},
          {2, "test.impure.shared", {}, {}},
          {3, "test.impure.copy", {WorkflowNodeOutput{1, "value"}}, {}},
          {4, "test.impure.copy", {WorkflowNodeOutput{2, "value"}}, {}}};
      doc.outputs = {{"a", 1, "value"},
                     {"b", 2, "value"},
                     {"c", 3, "value"},
                     {"d", 4, "value"}};
      GraphContext graph(doc);
      auto plan =
          Compiler(registry)
              .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
              .take_value()
              .plan.value();
      auto frozen = context->freeze(plan, {}).take_value();
      auto point = request({0});
      ExecutionOptions options;
      options.maximum_parallelism = 1;
      options.maximum_dependency_cache_work = 0;
      const DemandQuery demand{{"a", request({0, 1, 2, 3, 4, 5, 6, 7})},
                               {"b", request({0, 1, 2, 3, 4, 5, 6, 7})},
                               {"c", point},
                               {"d", point}};
      auto first = context->execute_fragments(frozen, demand, {}, options);
      require(first.ok(), first.status().message);
      require(*calls == 2,
              "identical non-pure nodes each execute once within a Run");
      auto second = context->execute_fragments(frozen, demand, {}, options);
      require(second.ok(), second.status().message);
      require(*calls == 4,
              "non-pure closure recomputes while prior Result owners survive");
      const auto read = [&](const DemandResult& execution, const char* name) {
        const auto& output = execution.results.at(name);
        double value = 0;
        require(output
                    .read_tensor(output.descriptor().take_value(), 0, {0, 0},
                                 &value, sizeof(value))
                    .ok(),
                "non-pure shared Result payload read");
        return value;
      };
      for (const auto* execution : {&first.value(), &second.value()}) {
        const auto a = read(*execution, "a"), b = read(*execution, "b");
        const auto minimum = execution == &first.value() ? 1.0 : 3.0;
        require((a == minimum && b == minimum + 1) ||
                    (b == minimum && a == minimum + 1),
                "independent non-pure producers publish distinct Run values");
        require(read(*execution, "c") == a && read(*execution, "d") == b,
                "pure consumers retain their own non-pure producer results");
      }
    }
    context.reset();
    zero(root);
  }
}
void result_checkpoints() {
  for (auto mode : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U}) {
    auto observer = std::make_shared<CheckpointObserver>();
    auto chunks = std::make_shared<ChunkObserver>();
    auto schema =
        chunk_schema(8, ElementType::Float64, "test.checkpoint.input");
    auto registry = std::make_shared<OperationRegistry>();
    OperationDefinition copy;
    copy.key = "checkpoint.copy";
    copy.traits = unified_result_fixture::traits(1, sizeof(ChunkCopy));
    unified_result_fixture::result_port(&copy.traits.input_schema[0], schema);
    unified_result_fixture::result_output(&copy.traits.outputs[0], schema);
    copy.map_dependencies = copy_mapping;
    copy.start_result = [chunks](const auto&, const auto& allocator) {
      return ResultContinuation::make<ChunkCopy>(allocator, chunks);
    };
    require(registry->register_operation(std::move(copy)).ok(),
            "checkpoint copy registration");
    auto dynamic_calls = std::make_shared<std::atomic<unsigned>>(0);
    if (mode == 9) {
      OperationDefinition dynamic;
      dynamic.key = "checkpoint.dynamic";
      dynamic.traits =
          unified_result_fixture::traits(0, sizeof(DynamicCheckpointSource));
      unified_result_fixture::result_output(&dynamic.traits.outputs[0], schema);
      dynamic.traits.deterministic = dynamic.traits.side_effect_free =
          dynamic.traits.cacheable = false;
      dynamic.start_result = [dynamic_calls](const auto&,
                                             const auto& allocator) {
        return ResultContinuation::make<DynamicCheckpointSource>(allocator,
                                                                 dynamic_calls);
      };
      require(registry
                  ->register_operation(
                      sdk::with_whole_stage(std::move(dynamic), 1000000))
                  .ok(),
              "dynamic Result producer registration");
    }
    OperationDefinition sum;
    sum.key = "checkpoint.sum";
    sum.traits = unified_result_fixture::traits(1, sizeof(CheckpointSum));
    unified_result_fixture::result_port(&sum.traits.input_schema[0], schema);
    unified_result_fixture::result_output(&sum.traits.outputs[0], schema);
    sum.map_dependencies = prefix_mapping;
    sum.start_result = [observer](const auto&, const auto& allocator) {
      return ResultContinuation::make<CheckpointSum>(allocator, observer);
    };
    require(registry->register_operation(std::move(sum)).ok() &&
                registry->freeze().ok(),
            "checkpoint sum registration");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    auto context = std::make_unique<ExecutionContext>(registry, config);
    auto root = context->resource_budget().take_value();
    {
      auto make_source = [&](const ResourceBudget& budget, double bias = 0) {
        auto builder =
            ResultBuilder::start(budget, schema, "input").take_value();

        double values[8];
        for (uint64_t i = 0; i < 8; ++i)
          values[i] = i + 1 + bias;
        require(builder
                    .publish_tensor(
                        0, Region::whole({1, 8}),
                        ByteView(reinterpret_cast<const uint8_t*>(values), 64))
                    .ok(),
                "checkpoint input samples");
        return builder.seal().take_value();
      };
      ExecutionBinding binding;
      binding.name = "source";
      binding.result = make_source(root);
      observer->bad_read = mode == 1;
      observer->bad_state = mode == 2;
      observer->block_retention = mode == 5;
      if (observer->bad_state)
        observer->foreign = make_source(ResourceBudget{});
      CancellationSource cancelled;
      if (mode == 3)
        observer->cancel = &cancelled;
      WorkflowDocument doc;
      WorkflowInputDeclaration input;
      input.id = 1;
      input.name = "source";
      input.result_schema = std::make_shared<SchemaTemplate>(schema);
      doc.inputs = {input};
      doc.nodes = {{1, "checkpoint.copy", {WorkflowInputReference{1}}, {}},
                   {2, "checkpoint.sum", {WorkflowNodeOutput{1, "value"}}, {}},
                   {3, "checkpoint.sum", {WorkflowNodeOutput{1, "value"}}, {}},
                   {4, "checkpoint.sum", {WorkflowNodeOutput{1, "value"}}, {}}};
      if (mode == 9)
        doc.nodes[0] = {1, "checkpoint.dynamic", {}, {}};
      if (mode == 8) {
        doc.nodes.clear();
        for (uint64_t i = 1; i <= 12; ++i)
          doc.nodes.push_back(
              {i,
               "checkpoint.copy",
               {i == 1 ? WorkflowInput{WorkflowInputReference{1}}
                       : WorkflowInput{WorkflowNodeOutput{i - 1, "value"}}},
               {}});
        for (uint64_t i = 2; i <= 4; ++i)
          doc.nodes.push_back({i + 20,
                               "checkpoint.sum",
                               {WorkflowNodeOutput{12, "value"}},
                               {}});
      }
      doc.outputs = {{"a", 2, "value"}, {"b", 3, "value"}, {"c", 4, "value"}};
      if (mode == 8)
        doc.outputs = {{"a", 22, "value"},
                       {"b", 23, "value"},
                       {"c", 24, "value"}};
      GraphContext graph(doc);
      auto plan =
          Compiler(registry)
              .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
              .take_value()
              .plan.value();
      auto frozen = context->freeze(plan, {{binding}}).take_value();
      const auto point = [](uint64_t at) { return request({at}); };
      ExecutionOptions options;
      options.maximum_parallelism = 1;
      if (mode == 4)
        options.maximum_dependency_cache_work = 0;
      if (mode == 8)
        options.maximum_dependency_cache_work = 1200;
      if (mode == 1 || mode == 3 || mode == 6 || mode == 7 || mode == 9) {
        ExecutionBinding alternate = binding;
        if (mode == 6)
          alternate.result = make_source(root, 100);
        auto other = mode == 6
                         ? context->freeze(plan, {{alternate}}).take_value()
                         : frozen;
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false, release = false, finished = false;
        auto held_options = options;
        held_options.result_publication = [&](ValueRef ref, const ResultRef&) {
          if (ref.node_id != 2)
            return Status::success();
          std::unique_lock<std::mutex> lock(mutex);
          entered = true;
          changed.notify_all();
          if (!changed.wait_for(lock, std::chrono::seconds(5),
                                [&] { return release; }))
            return Status{ErrorCode::Cancelled, "checkpoint barrier timed out"};
          return Status::success();
        };
        auto first = std::async(std::launch::async, [&] {
          auto result = context->execute_fragments(frozen, {{"a", point(2)}},
                                                   {}, held_options);
          {
            std::lock_guard<std::mutex> lock(mutex);
            finished = true;
          }
          changed.notify_all();
          return result;
        });
        {
          std::unique_lock<std::mutex> lock(mutex);
          require(changed.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return entered || finished; }),
                  "completed checkpoint scope remains active at barrier");
          if (!entered) {
            lock.unlock();
            auto result = first.get();
            require(result.ok(),
                    "checkpoint mode=" + std::to_string(mode) +
                        " before barrier: " + result.status().message);
            require(false, "checkpoint publication observer was not called");
          }
        }
        auto second = context->execute_fragments(
            other, {{mode == 9 ? "b" : "a", point(mode == 9 ? 5 : 2)}},
            cancelled.token());
        {
          std::lock_guard<std::mutex> lock(mutex);
          release = true;
        }
        changed.notify_all();
        auto completed = first.get();
        require(completed.ok(), completed.status().message);
        if (mode == 1 || mode == 3) {
          require(
              !second.ok() && second.status().code ==
                                  (mode == 3 ? ErrorCode::Cancelled
                                             : ErrorCode::InvalidArgument),
              "ignored checkpoint failures after actual restore are sticky");
          require(observer->restored == std::vector<uint64_t>{2},
                  "negative checkpoint probe restores the completed state");
        } else {
          require(second.ok(), second.status().message);
          const auto& output = second.value().results.at(mode == 9 ? "b" : "a");
          double actual = 0;
          require(
              output.read_tensor(output.descriptor().take_value(), 0,
                                 {0, mode == 9 ? 5U : 2U}, &actual, 8)
                      .ok() &&
                  actual == (mode == 6   ? 306.0
                             : mode == 9 ? 27.0
                                         : 6.0),
              "active frozen runs isolate bindings and share only completed "
              "state");
          require(observer->starts == (mode == 6 || mode == 9
                                           ? std::vector<uint64_t>{0, 0}
                                           : std::vector<uint64_t>{0, 3}),
                  "checkpoint sharing uses the full frozen input basis");
          auto units = dirty(second.value().dependencies, root, 0);
          require(mode == 9 ? units.empty() : !units.empty(),
                  "checkpoint restores source tile ancestry");
          if (mode == 7)
            require(chunks->produced.size() == 6,
                    "each run prepares its static prefix before checkpoint "
                    "restoration");
          if (mode == 9)
            require(*dynamic_calls == 2 && observer->restored.empty(),
                    "checkpoint sharing excludes an impure upstream closure");
        }
      } else {
        const DemandQuery requested = mode == 5 ? DemandQuery{{"a", point(2)}}
                                                : DemandQuery{{"a", point(2)},
                                                              {"b", point(5)},
                                                              {"c", point(0)}};
        auto result = context->execute_fragments(frozen, requested,
                                                 cancelled.token(), options);
        if (mode == 2) {
          require(!result.ok() && result.status().code ==
                                      (mode == 3 ? ErrorCode::Cancelled
                                                 : ErrorCode::InvalidArgument),
                  "ignored checkpoint failures are sticky");
        } else {
          require(result.ok(), result.status().message);
          const auto queries = mode == 5 ? 1U : 3U;
          require(observer->starts.size() == queries,
                  "each requested checkpoint query starts once");
          require(observer->targets.size() == queries,
                  "checkpoint cursors retain their requested targets");
          std::vector<uint64_t> restored;
          for (size_t i = 0; i < queries; ++i) {
            require(observer->starts[i] <= observer->targets[i] + 1,
                    "checkpoint cursor never restores a future sequence");
            if (observer->starts[i])
              restored.push_back(observer->starts[i] - 1);
          }
          require(observer->restored == restored,
                  "restored phase and sequence agree with computation cursors");
          require(observer->starts == std::vector<uint64_t>(queries, 0) &&
                      observer->restored.empty(),
                  "different computation units do not share checkpoint state");
          for (const auto& entry :
               {std::make_pair("a", 6.0), std::make_pair("b", 21.0),
                std::make_pair("c", 1.0)}) {
            if (mode == 5 && entry.first != std::string("a"))
              continue;
            const auto& output = result.value().results.at(entry.first);
            const auto at = entry.first == std::string("a")   ? 2U
                            : entry.first == std::string("b") ? 5U
                                                              : 0U;
            double actual = 0;
            require(output.read_tensor(output.descriptor().take_value(), 0,
                                       {0, at}, &actual, 8)
                            .ok() &&
                        actual == entry.second,
                    "checkpoint result values");
          }
          for (uint64_t edited : {0U, 2U, 4U, 7U}) {
            auto units = dirty(result.value().dependencies, root, edited);
            const auto is_dirty = [&](const char* name, TileIndex tile) {
              return std::find(units.begin(), units.end(),
                               TileUnit{plan.output(name).record_index, 0, 0,
                                        tile}) != units.end();
            };
            require(is_dirty("a", 2) == (edited <= 2) &&
                        (mode == 5 || (is_dirty("b", 5) == (edited <= 5) &&
                                       is_dirty("c", 0) == (edited == 0))),
                    "checkpoint restores complete prefix tile dependencies");
          }
          const auto depth = mode == 8 ? 12U : 1U;
          require(chunks->produced.size() >= (mode == 5 ? 3U : 6U) * depth &&
                      chunks->produced.size() <= (mode == 5 ? 3U : 10U) * depth,
                  "parallel queries compute only requested source prefixes");
          require(result.value().diagnostics.dependency_cache_work <=
                      options.maximum_dependency_cache_work,
                  "checkpoint ancestor copies remain in optional work budget");
        }
      }
      for (const auto& weak : chunks->produced)
        require(!weak.lock().valid(),
                "checkpoint history retains no chunk payload");
    }
    context.reset();
    if (observer->retained.valid()) {
      double value = 0;
      auto state = observer->retained.state();
      require(
          state.read_tensor(state.descriptor().take_value(), 0, {0}, &value, 8)
                  .ok() &&
              (value == 1 || value == 6),
          "owning checkpoint state survives execution context retirement");
    }
    observer->retained = {};
    observer->foreign = {};
    zero(root);
  }
}
void tuple_structure() {
  auto schema = unified_result_fixture::image_schema();
  auto& image = schema.tensors[0];
  for (uint64_t channels : {1, 3, 4, 7}) {
    image.descriptor = {ElementType::Float32, {2, 4, channels}};
    image.layout.channel_axis = 2;
    image.atomic_trailing_axes = 1;
    require(schema.validate().ok(), "arbitrary channel count");
    auto q = Footprint::from_regions(
                 {2, 2, 2, 4, channels},
                 {Region({{1, 1}, {0, 1}, {1, 1}, {2, 1}, {0, 1}})})
                 .take_value();
    require(image.close_samples(q).value().element_count().value() == channels,
            "explicit atomic trailing axis closure");
    image.atomic_trailing_axes = 0;
    require(image.close_samples(q).value() == q, "independent channel reads");
  }
  image.descriptor = {ElementType::Float32, {6, 2, 4}};
  image.layout.channel_axis = 0;
  image.layout.height_axis = 1;
  image.layout.width_axis = 2;
  require(schema.validate().ok(), "channel-first spatial slot");
  auto q =
      Footprint::from_regions(
          {2, 2, 6, 2, 4}, {Region({{1, 1}, {0, 1}, {2, 1}, {1, 1}, {2, 1}})})
          .take_value();
  require(image.close_samples(q).value() == q,
          "channel-first independent read");
}
void storage_owner() {
  ResourceBudget root;
  ResultRef survivor;
  {
    auto schema = unified_result_fixture::image_schema();
    schema.id = "test.profile";
    auto& slot = schema.tensors[0];
    slot.batch_axes[0] = slot.batch_axes[1] = 1;
    slot.descriptor.shape = {1, 1, 4};
    slot.layout.channel_axis = 2;
    auto registry = std::make_shared<OperationRegistry>();
    require(join(registry.get(), schema).ok(), "profile join");
    require(registry->freeze().ok(), "profile freeze");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    root = context.resource_budget().take_value();
    auto destination = root;
    auto input = image(destination, schema);
    WorkflowDocument doc;
    WorkflowInputDeclaration declaration;
    declaration.id = 1;
    declaration.name = "source";
    declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
    doc.inputs = {declaration};
    doc.nodes = {{1,
                  "test.image.join",
                  {WorkflowInputReference{1}, WorkflowInputReference{1}},
                  {}}};
    doc.outputs = {{"out", 1, "value"}};
    GraphContext graph(doc);
    auto plan = Compiler(registry).compile(graph);
    require(plan.ok(), plan.status().message);
    ExecutionBinding binding;
    binding.name = "source";
    binding.result = input;
    auto result = context.execute(plan.value().plan.value(), {{binding}});
    require(result.ok(), result.status().message);
    survivor = result.value().results.at("out");
  }
  float read = 0;
  require(survivor.read_tensor(survivor.descriptor().value(), 0,
                               {0, 0, 0, 0, 3}, &read, 4)
                  .ok() &&
              read == 6,
          "image backing outlives context");
  survivor = {};
  zero(root);
}
}  // namespace
int main() {
  try {
    resource_admission();
    staged_input_retirement();
    result_blocks();
    result_checkpoints();
    impure_shared_results();
    tuple_structure();
    alias_diamond();
    all_member_acquisition();
    storage_owner();
    std::cout << "Result image contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
