#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "photospider/plugin/tile_helpers.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct LazyProgram {
  unsigned mode = 0, stage = 0;
  double value;
  LazyProgram(unsigned mode, double value) : mode(mode), value(value) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    using multi_result::check;
    using multi_result::take;
    if (mode == 3 && stage == 0) {
      stage = 1;
      ResultProgramNeed need;
      need.tiles.push_back({1, true, false, {}});
      need.tiles.push_back({0, false, false, {}});
      return Answer(std::move(need));
    }
    if (mode && stage == 0) {
      stage = 1;
      ResultProgramNeed need;
      {
        auto selected = sdk::tile_need_for_samples(
            phase.resources, phase.query, 0, 0, take(Footprint::all({1})),
            ((1) & 7U) == 0);
        if (!selected.ok())
          return Result<ResultProgramPoll>(selected.status());
        need.tiles.push_back(selected.take_value());
      }
      return Answer(std::move(need));
    }
    if (stage == 1) {
      float control = 0;
      check(phase.read_tensor(0, 0, {0}, &control, sizeof(control)));
      stage = 2;
      if (control != 0) {
        ResultProgramNeed need;
        if (mode == 2) {
          need.tiles.push_back({1, true, false, {}});
        } else {
          auto selected = sdk::tile_need_for_samples(
              phase.resources, phase.query, 1, 0, take(Footprint::all({1})),
              ((1) & 7U) == 0);
          if (!selected.ok())
            return Result<ResultProgramPoll>(selected.status());
          need.tiles.push_back(selected.take_value());
        }
        return Answer(std::move(need));
      }
    }
    // Do not read the data grant. Its entire scalar port still needs
    // validation.
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{}));
    const auto& spec = phase.query.output.result_schema->tensors[0];
    std::uint8_t bytes[8]{};
    multi_result::write_number(bytes, spec.descriptor.element_type, value);
    check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(bytes, multi_result::width(spec.descriptor.element_type))));
    return Answer(ResultPublication{take(builder.seal()), true});
  }
};
struct Fixture {
  std::shared_ptr<OperationRegistry> operations =
      std::make_shared<OperationRegistry>();
  SchemaTemplate scalar = multi_result::schema(ElementType::Float32);
  std::atomic<unsigned> producer_calls{0}, consumer_calls{0};
  Fixture(unsigned mode, bool projected, bool producer_failure, double value) {
    OperationDefinition producer;
    producer.key = "test.lazy_source";
    producer.traits.outputs = {multi_result::output("value", scalar)};
    producer.traits.outputs[0].region_rule = OperationRegionRule::Whole;
    producer.traits.cacheable = false;
    producer.start_result = [this, producer_failure, value](
                                const auto&, const auto& allocator) {
      ++producer_calls;
      if (producer_failure)
        return Result<ResultContinuation>(
            Status{ErrorCode::OperationFailed, "unrequested producer"});
      return ResultContinuation::make<LazyProgram>(allocator, 0, value);
    };
    multi_result::check(operations->register_operation(std::move(producer)));
    OperationDefinition consumer;
    consumer.key = "test.lazy_whole";
    consumer.traits.input_count = 2;
    consumer.traits.input_schema.resize(2);
    for (auto& port : consumer.traits.input_schema) {
      port.kind = OperationPortKind::Result;
      port.tensor_key = "number";
      port.element_type = static_cast<unsigned>(ElementType::Float32);
      port.rank = 1;
      port.scalar_bounds = true;
      port.minimum = 0;
      port.maximum = 1;
    }
    consumer.traits.outputs = {multi_result::output("value")};
    consumer.traits.outputs[0].region_rule = OperationRegionRule::Whole;
    consumer.traits.outputs[0].maximum_dependency_stages = 3;
    if (projected)
      consumer.traits.outputs[0].input_indices = std::vector<std::uint32_t>{0};
    consumer.traits.cacheable = false;
    consumer.start_result = [this, mode](const auto&, const auto& allocator) {
      ++consumer_calls;
      return ResultContinuation::make<LazyProgram>(allocator, mode, 7.0);
    };
    multi_result::check(operations->register_operation(std::move(consumer)));
    multi_result::check(operations->freeze());
  }
  WorkflowDocument document(bool computed) const {
    WorkflowDocument result;
    result.inputs = {multi_result::declaration(1, "control", scalar)};
    WorkflowInput data;
    if (computed) {
      result.nodes.push_back({1, "test.lazy_source", {}, {}});
      data = WorkflowNodeOutput{1, "value"};
    } else {
      result.inputs.push_back(multi_result::declaration(2, "data", scalar));
      data = WorkflowInputReference{2};
    }
    result.nodes.push_back(
        {2, "test.lazy_whole", {WorkflowInputReference{1}, data}, {}});
    result.outputs = {{"value", 2, "value"}};
    return result;
  }
};
int run_cases() {
  for (unsigned mode : {0U, 1U, 2U}) {
    for (bool computed : {false, true}) {
      Fixture fixture(mode, false, true, 2);
      GraphContext graph(fixture.document(computed));
      auto compiled = Compiler(fixture.operations).compile(graph);
      PS_REQUIRE_OK(compiled);
      ExecutionContext context(fixture.operations);
      auto root = context.resource_budget().take_value();
      ExecutionBindings inputs{{multi_result::binding(
          root, "control", mode ? 0 : NAN, fixture.scalar)}};
      if (!computed)
        inputs.inputs.push_back(
            multi_result::binding(root, "data", NAN, fixture.scalar));
      auto frozen = context.freeze(compiled.value().plan, inputs);
      PS_REQUIRE_OK(frozen);
      auto result = context.execute(frozen.value());
      PS_REQUIRE_OK(result);
      PS_CHECK(multi_result::number(result.value().results.at("value")) == 7);
      PS_CHECK(fixture.producer_calls == 0 && fixture.consumer_calls == 1);
    }
  }
  for (unsigned mode : {1U, 2U}) {
    for (bool computed : {false, true}) {
      for (double value : std::vector<double>{.5, 2.0, NAN}) {
        Fixture fixture(mode, false, false, value);
        GraphContext graph(fixture.document(computed));
        auto compiled = Compiler(fixture.operations).compile(graph);
        PS_REQUIRE_OK(compiled);
        ExecutionContext context(fixture.operations);
        auto root = context.resource_budget().take_value();
        ExecutionBindings inputs{
            {multi_result::binding(root, "control", 1, fixture.scalar)}};
        if (!computed)
          inputs.inputs.push_back(
              multi_result::binding(root, "data", value, fixture.scalar));
        auto frozen = context.freeze(compiled.value().plan, inputs);
        PS_REQUIRE_OK(frozen);
        auto result = context.execute(frozen.value());
        if (value == .5) {
          PS_REQUIRE_OK(result);
          PS_CHECK(multi_result::number(result.value().results.at("value")) ==
                   7);
        } else {
          PS_CHECK(result.status().code == (computed
                                                ? ErrorCode::OperationFailed
                                                : ErrorCode::InvalidArgument));
          PS_CHECK(computed ? result.status().detail.node_id == 1
                            : result.status().detail.input_id == 2);
        }
        PS_CHECK(fixture.producer_calls == unsigned(computed));
      }
    }
    Fixture fixture(mode, true, true, 2);
    GraphContext graph(fixture.document(true));
    auto compiled = Compiler(fixture.operations).compile(graph);
    PS_REQUIRE_OK(compiled);
    ExecutionContext context(fixture.operations);
    auto root = context.resource_budget().take_value();
    auto rejected = context.execute(
        compiled.value().plan,
        {{multi_result::binding(root, "control", 1, fixture.scalar)}});
    PS_CHECK(rejected.status().code == ErrorCode::InvalidArgument &&
             rejected.status().detail.origin == FailureOrigin::Protocol &&
             fixture.producer_calls == 0);
  }
  return 0;
}
int resolved_whole() {
  for (bool spatial : {false, true}) {
    auto schema = multi_result::schema(
        ElementType::Float64, spatial ? std::vector<std::uint64_t>{3, 5, 1}
                                      : std::vector<std::uint64_t>{4});
    schema.tensors[0].layout.spatial = spatial;
    auto operations = std::make_shared<OperationRegistry>();
    std::atomic<unsigned> calls{0};
    OperationDefinition operation;
    operation.key = "test.resolved_whole";
    operation.traits.outputs = {multi_result::output("value", schema)};
    operation.traits.workspace_bytes =
        schema.tensors[0].sample_count().value() * 8;
    operation.traits.outputs[0].region_rule =
        spatial ? OperationRegionRule::Whole : OperationRegionRule::Tiled;
    operation.start_result = [&](const auto&, const auto& allocator) {
      ++calls;
      return ResultContinuation::make<multi_result::Program>(allocator, -1,
                                                             4.0);
    };
    PS_CHECK(operations->register_operation(std::move(operation)).ok());
    PS_CHECK(operations->freeze().ok());
    WorkflowDocument document;
    document.nodes = {{1, "test.resolved_whole", {}, {}}};
    document.outputs = {{"first", 1, "value"}, {"last", 1, "value"}};
    GraphContext graph(std::move(document));
    auto compiled = Compiler(operations).compile(graph);
    PS_REQUIRE_OK(compiled);
    const auto& step = compiled.value().plan.steps()[0];
    PS_CHECK(!step.tiled);
    PS_CHECK(step.tile_grid.has_value() == spatial);
    ExecutionContext context(operations);
    auto frozen = context.freeze(compiled.value().plan, {});
    PS_REQUIRE_OK(frozen);
    const auto shape = schema.tensors[0].sample_shape();
    auto root = context.resource_budget().take_value();
    const auto request =
        spatial
            ? TileRequest{false, TileSet::from_indices(root, {0}).take_value()}
            : TileRequest{true, {}};
    auto result = context.execute_fragments(
        frozen.value(), {{"first", request}, {"last", request}});
    PS_REQUIRE_OK(result);
    PS_CHECK(calls == 1);
    for (const auto& output : result.value().results) {
      auto descriptor = output.second.descriptor();
      PS_REQUIRE_OK(descriptor);
      PS_CHECK(descriptor.value().tensor_coverage(0) ==
               Footprint::all(shape).take_value());
      PS_CHECK(multi_result::number(output.second,
                                    spatial
                                        ? std::vector<std::uint64_t>{2, 4, 0}
                                        : std::vector<std::uint64_t>{3}) == 4);
    }
  }
  return 0;
}
int planned_tiles() {
  auto schema = multi_result::schema(ElementType::Float64, {10, 10, 2});
  schema.tensors[0].layout.spatial = true;
  auto operations = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "test.planned_tiles";
  operation.traits.outputs = {multi_result::output("value", schema)};
  operation.traits.workspace_bytes = 1600;
  operation.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<multi_result::Program>(allocator, -1, 6.0);
  };
  PS_CHECK(operations->register_operation(std::move(operation)).ok());
  PS_CHECK(operations->freeze().ok());
  WorkflowDocument document;
  document.nodes = {{1, "test.planned_tiles", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(std::move(document));
  ExecutionContext context(operations);
  const auto root = context.resource_budget().take_value();
  PlanningOptions options;
  options.tile_extent = 4;
  options.output_requests = {
      {"value", {false, TileSet::from_indices(root, {8, 4, 8}).take_value()}}};
  auto compiled = Compiler(operations).compile(graph, options);
  PS_REQUIRE_OK(compiled);
  auto run = context.execute(compiled.value().plan);
  PS_REQUIRE_OK(run);
  const auto expected =
      Footprint::from_regions({10, 10, 2}, {Region({{4, 4}, {4, 4}, {0, 2}}),
                                            Region({{8, 2}, {8, 2}, {0, 2}})})
          .take_value();
  const auto& value = run.value().results.at("value");
  PS_CHECK(value.descriptor().value().tensor_coverage(0) == expected);
  PS_CHECK(multi_result::number(value, {9, 9, 1}) == 6);
  double unavailable = 0;
  PS_CHECK(!value
                .read_tensor(value.descriptor().value(), 0, {0, 0, 0},
                             &unavailable, 8)
                .ok());
  auto empty = compiled.value().plan.tile_plan("value", {});
  PS_REQUIRE_OK(empty);
  auto empty_run = context.execute(empty.value());
  PS_REQUIRE_OK(empty_run);
  PS_CHECK(empty_run.value()
               .results.at("value")
               .descriptor()
               .value()
               .tensor_coverage(0)
               .empty());
  auto frozen = context.freeze(compiled.value().plan, {}).take_value();
  auto last = frozen.for_tiles(
      "value", {false, TileSet::from_indices(root, {8}).take_value()});
  PS_REQUIRE_OK(last);
  auto last_run = context.execute(last.value());
  PS_REQUIRE_OK(last_run);
  PS_CHECK(
      last_run.value().results.at("value").descriptor().value().tensor_coverage(
          0) ==
      Footprint::from_regions({10, 10, 2}, {Region({{8, 2}, {8, 2}, {0, 2}})})
          .take_value());
  return 0;
}
struct UnitPublicationProgram {
  unsigned mode, stage = 0;
  ResultBuilder builder;
  explicit UnitPublicationProgram(unsigned mode) : mode(mode) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using multi_result::check;
    using multi_result::take;
    using Answer = Result<ResultProgramPoll>;
    if (stage && mode == 1)
      return Answer(
          Status{ErrorCode::OperationFailed, "failure after partial tile"});
    auto schema = *phase.query.output.result_schema;
    schema.tensors[0].layout.order = ImagePlaneOrder::Continuous;
    schema.tensors[0].layout.row_pitch_bytes = 64;
    if (!stage) {
      builder = take(ResultBuilder::start(phase.resources, schema,
                                          phase.query.semantic_key, {}, {},
                                          phase.query.tile_extent));
    }
    auto region = mode == 3 ? Region::whole(schema.tensors[0].sample_shape())
                            : phase.query.tensor_outputs->boxes()[0];
    if (mode == 1 || mode == 2 || mode == 4) {
      auto dimensions = region.dimensions();
      if (stage) {
        ++dimensions[1].offset;
        --dimensions[1].extent;
      } else {
        dimensions[1].extent = 1;
      }
      region = Region(std::move(dimensions));
    }
    const auto count = take(region.element_count());
    double number = 1.0 + phase.query.tile_unit;
    if (mode == 5) {
      auto found = take(phase.checkpoint_before(1, 1));
      if (found) {
        number = multi_result::number(found->state());
      } else {
        auto state =
            multi_result::binding(phase.resources, "state", number).result;
        check(phase.checkpoint_publish(1, 1, state));
      }
    }
    std::vector<double> values(count, number);
    check(builder.publish_tensor(
        0, region,
        ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                 count * sizeof(double))));
    if (!stage++ && (mode == 1 || mode == 2))
      return Answer(ResultPublication{builder.reference(), false});
    return Answer(ResultPublication{take(builder.seal()), true});
  }
};
int atomic_unit_publications() {
  for (bool whole : {false, true}) {
    for (unsigned mode : {0U, 1U, 2U, 3U, 4U, 5U}) {
      if (whole && mode == 3)
        continue;
      auto schema = multi_result::schema(ElementType::Float64, {1, 4, 2});
      schema.tensors[0].layout.spatial = true;
      schema.publication = PublishPolicy::IndependentChunks;
      auto operations = std::make_shared<OperationRegistry>();
      OperationDefinition source;
      source.key = "test.unit_publication";
      source.traits.outputs = {multi_result::output("value", schema)};
      if (whole)
        source.traits.outputs[0].region_rule = OperationRegionRule::Whole;
      source.traits.outputs[0].continuation_bytes = 1024;
      source.traits.cacheable = false;
      source.start_result = [mode](const auto&, const auto& allocator) {
        return ResultContinuation::make<UnitPublicationProgram>(allocator,
                                                                mode);
      };
      PS_CHECK(operations->register_operation(std::move(source)).ok());
      PS_CHECK(operations->freeze().ok());
      WorkflowDocument document;
      document.nodes = {{1, "test.unit_publication", {}, {}}};
      document.outputs = {{"value", 1, "value"}};
      GraphContext graph(std::move(document));
      PlanningOptions planning;
      planning.tile_extent = 2;
      auto compiled = Compiler(operations).compile(graph, planning);
      PS_REQUIRE_OK(compiled);
      ExecutionContext context(operations);
      unsigned notifications = 0;
      std::vector<ResultRef> published;
      ExecutionOptions options;
      options.maximum_parallelism = 1;
      options.result_publication = [&](ValueRef, const ResultRef& value) {
        auto facts = value.descriptor();
        if (!facts.ok() || !facts.value().sealed() ||
            facts.value().tensor_coverage(0).element_count().value() !=
                (whole ? 8 : 4))
          return Status{ErrorCode::OperationFailed, "partial unit was exposed"};
        ++notifications;
        published.push_back(value);
        return Status::success();
      };
      auto frozen = context.freeze(compiled.value().plan, {});
      PS_REQUIRE_OK(frozen);
      auto result = context.execute(frozen.value(), {}, options);
      if (mode == 1 || mode == 3 || mode == 4) {
        PS_CHECK(!result.ok() && notifications == 0);
        if (result.status().code != (mode == 1 ? ErrorCode::OperationFailed
                                               : ErrorCode::InvalidArgument)) {
          std::cerr << "unit mode=" << mode << " whole=" << whole
                    << " code=" << static_cast<int>(result.status().code)
                    << " message=" << result.status().message << '\n';
          return 1;
        }
      } else {
        PS_REQUIRE_OK(result);
        PS_CHECK(notifications == (whole ? 1 : 2));
        const auto& value = result.value().results.at("value");
        PS_CHECK(value.descriptor().value().tensor_coverage(0) ==
                 Footprint::all({1, 4, 2}).value());
        PS_CHECK(multi_result::number(value, {0, 3, 1}) == (whole ? 1 : 2));
        for (const auto& unit : published) {
          auto facts = unit.descriptor().take_value();
          const auto region = facts.tensor_coverage(0).boxes()[0];
          auto original = unit.acquire_tensor(facts, 0, region);
          auto assembled =
              value.acquire_tensor(value.descriptor().value(), 0, region);
          PS_REQUIRE_OK(original);
          PS_REQUIRE_OK(assembled);
          std::vector<std::uint64_t> coordinate;
          for (const auto& dimension : region.dimensions())
            coordinate.push_back(dimension.offset);
          auto source_row = original.value().row_run(coordinate);
          auto output_row = assembled.value().row_run(coordinate);
          PS_REQUIRE_OK(source_row);
          PS_REQUIRE_OK(output_row);
          PS_CHECK(source_row.value().data == output_row.value().data);
        }
      }
    }
  }
  return 0;
}
struct RepeatedTileConsumer {
  unsigned stage = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using multi_result::check;
    using multi_result::take;
    if (stage < 4) {
      const std::vector<TileIndex> indices =
          stage == 3 ? std::vector<TileIndex>{0, 1}
                     : std::vector<TileIndex>{stage == 1 ? 1U : 0U};
      ++stage;
      ResultProgramNeed need;
      need.tiles.push_back(
          {0, false, false,
           take(TileSet::from_indices(phase.resources, indices))});
      return Result<ResultProgramPoll>(std::move(need));
    }
    double sum = 0;
    for (std::uint64_t x = 0; x < 4; ++x)
      for (std::uint64_t c = 0; c < 2; ++c) {
        double value = 0;
        check(phase.read_tensor(0, 0, {0, x, c}, &value, sizeof(value)));
        sum += value;
      }
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));

    check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&sum), sizeof(sum))));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
int repeated_tile_needs() {
  auto schema = multi_result::schema(ElementType::Float64, {1, 4, 2});
  schema.tensors[0].layout.spatial = true;
  auto operations = std::make_shared<OperationRegistry>();
  std::atomic<unsigned> starts[2]{};
  OperationDefinition source;
  source.key = "test.unit_source";
  source.traits.outputs = {multi_result::output("value", schema)};
  source.traits.cacheable = false;
  source.traits.workspace_bytes = 1024;
  source.start_result = [&](const auto& query, const auto& allocator) {
    if (query.output_request.whole || query.output_request.tiles.size() != 1 ||
        query.tile_unit > 1 ||
        *query.output_request.tiles.begin() != query.tile_unit)
      return Result<ResultContinuation>(
          Status{ErrorCode::OperationFailed, "invalid tile query"});
    ++starts[query.tile_unit];
    return ResultContinuation::make<multi_result::Program>(
        allocator, -1, 1.0 + query.tile_unit);
  };
  PS_CHECK(operations->register_operation(std::move(source)).ok());
  OperationDefinition consumer;
  consumer.key = "test.unit_consumer";
  consumer.traits.input_count = 1;
  consumer.traits.input_schema.resize(1);
  consumer.traits.input_schema[0].kind = OperationPortKind::Result;
  consumer.traits.input_schema[0].tensor_key = "number";
  consumer.traits.outputs = {multi_result::output("value")};
  consumer.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  consumer.traits.outputs[0].maximum_dependency_stages = 5;
  consumer.traits.cacheable = false;
  consumer.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<RepeatedTileConsumer>(allocator);
  };
  PS_CHECK(operations->register_operation(std::move(consumer)).ok());
  PS_CHECK(operations->freeze().ok());
  WorkflowDocument document;
  document.nodes = {
      {1, "test.unit_source", {}, {}},
      {2, "test.unit_consumer", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"sum", 2, "value"}};
  GraphContext graph(std::move(document));
  PlanningOptions options;
  options.tile_extent = 2;
  auto compiled = Compiler(operations).compile(graph, options);
  PS_REQUIRE_OK(compiled);
  ExecutionContext context(operations);
  auto result = context.execute(compiled.value().plan);
  PS_REQUIRE_OK(result);
  PS_CHECK(multi_result::number(result.value().results.at("sum")) == 12);
  PS_CHECK(starts[0] == 1 && starts[1] == 1);
  const auto& dependencies = result.value().dependencies;
  auto record = dependencies.tile_record({1, 0, 0});
  PS_REQUIRE_OK(record);
  PS_CHECK(record.value() && record.value()->inputs.size() == 1);
  const auto& input = record.value()->inputs[0];
  PS_CHECK(!input.whole && input.tiles.size() == 2 && input.tiles.contains(0) &&
           input.tiles.contains(1));
  auto root = context.resource_budget().take_value();
  ResourceVector<TileChange> changes{ResourceAllocator<TileChange>(root)};
  changes.push_back({PlanStepInput{0}, 0,
                     TileSet::from_indices(root, {0}).take_value(), false});
  auto dirty = dependencies.dirty_units(changes);
  PS_REQUIRE_OK(dirty);
  PS_CHECK(dirty.value().size() == 1 &&
           (dirty.value()[0] == TileUnit{1, 0, 0}));
  for (TileIndex unit : {0, 1}) {
    auto source_record = dependencies.tile_record({0, 0, unit});
    PS_REQUIRE_OK(source_record);
    PS_CHECK(source_record.value() && source_record.value()->inputs.empty());
  }
  return 0;
}
struct CheckpointTileSource {
  bool requested = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using multi_result::check;
    using multi_result::take;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back({0, false, false, phase.query.output_request.tiles});
      return Result<ResultProgramPoll>(std::move(need));
    }
    const auto& region = phase.query.tensor_outputs->boxes()[0];
    double value = 0;
    check(phase.read_tensor(0, 0, {0, region.dimensions()[1].offset, 0}, &value,
                            sizeof(value)));
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    const auto count = take(region.element_count());
    std::vector<double> values(count, value);

    check(builder.publish_tensor(
        0, region,
        ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                 count * sizeof(value))));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
struct CheckpointTileGate {
  std::mutex mutex;
  std::condition_variable changed;
  bool published = false, release = false;
  std::atomic<bool> unauthorized{false};
};
struct CheckpointTileConsumer {
  explicit CheckpointTileConsumer(CheckpointTileGate* gate) : gate(gate) {}
  CheckpointTileGate* gate;
  unsigned stage = 0;
  double prefix = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using multi_result::check;
    using multi_result::take;
    if (!stage) {
      auto checkpoint = take(phase.checkpoint_before(1, 1));
      if (checkpoint) {
        prefix = multi_result::number(checkpoint->state());
        if (gate->unauthorized.load()) {
          double denied = 0;
          // Restored dependency evidence must not restore payload authority.
          (void)phase.read_tensor(0, 0, {0, 0, 0}, &denied, sizeof(denied));
        }
        stage = 2;
      } else {
        stage = 1;
        ResultProgramNeed need;
        need.tiles.push_back(
            {0, false, false,
             take(TileSet::from_indices(phase.resources, {0}))});
        return Result<ResultProgramPoll>(std::move(need));
      }
    }
    if (stage == 1) {
      check(phase.read_tensor(0, 0, {0, 0, 0}, &prefix, sizeof(prefix)));
      auto state =
          multi_result::binding(phase.resources, "checkpoint", prefix).result;
      check(phase.checkpoint_publish(1, 1, state));
      {
        std::unique_lock<std::mutex> lock(gate->mutex);
        if (!gate->published) {
          gate->published = true;
          gate->changed.notify_all();
          gate->changed.wait(lock, [&] { return gate->release; });
        }
      }
      stage = 2;
    }
    if (stage == 2) {
      stage = 3;
      ResultProgramNeed need;
      need.tiles.push_back(
          {0, false, false, take(TileSet::from_indices(phase.resources, {1}))});
      return Result<ResultProgramPoll>(std::move(need));
    }
    double suffix = 0;
    check(phase.read_tensor(0, 0, {0, 2, 0}, &suffix, sizeof(suffix)));
    LazyProgram output(0, prefix + suffix);
    return output.poll(phase);
  }
};
int checkpoint_tile_dependencies(bool cached = false) {
  auto schema = multi_result::schema(ElementType::Float64, {1, 4, 2});
  schema.tensors[0].layout.spatial = true;
  auto operations = std::make_shared<OperationRegistry>();
  std::atomic<unsigned> starts[2]{};
  OperationDefinition source;
  source.key = "test.checkpoint_source";
  source.traits.input_count = 1;
  source.traits.input_schema.resize(1);
  source.traits.input_schema[0].kind = OperationPortKind::Result;
  source.traits.input_schema[0].tensor_key = "number";
  source.traits.outputs = {multi_result::output("value", schema)};
  source.traits.cacheable = cached;
  source.start_result = [&](const auto& query, const auto& allocator) {
    ++starts[query.tile_unit];
    return ResultContinuation::make<CheckpointTileSource>(allocator);
  };
  PS_CHECK(operations->register_operation(std::move(source)).ok());
  OperationDefinition consumer;
  consumer.key = "test.checkpoint_tiles";
  consumer.traits.input_count = 1;
  consumer.traits.input_schema.resize(1);
  consumer.traits.input_schema[0].kind = OperationPortKind::Result;
  consumer.traits.input_schema[0].tensor_key = "number";
  consumer.traits.outputs = {multi_result::output("value")};
  consumer.traits.outputs[0].maximum_dependency_stages = 3;
  consumer.traits.cacheable = cached;
  CheckpointTileGate gate;
  consumer.start_result = [&](const auto&, const auto& allocator) {
    return ResultContinuation::make<CheckpointTileConsumer>(allocator, &gate);
  };
  PS_CHECK(operations->register_operation(std::move(consumer)).ok());
  PS_CHECK(operations->freeze().ok());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "image", schema)};
  document.nodes = {
      {1, "test.checkpoint_source", {WorkflowInputReference{1}}, {}},
      {2, "test.checkpoint_tiles", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"sum", 2, "value"}};
  GraphContext graph(std::move(document));
  PlanningOptions planning;
  planning.tile_extent = 2;
  auto compiled = Compiler(operations).compile(graph, planning);
  PS_REQUIRE_OK(compiled);
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.result_cache_bytes = cached ? 16 * 1024 * 1024 : 0;
  ExecutionContext context(operations, config);
  auto root = context.resource_budget().take_value();
  auto frozen =
      context.freeze(compiled.value().plan,
                     {{multi_result::binding(root, "image", 7, schema)}});
  PS_REQUIRE_OK(frozen);
  auto first = std::async(std::launch::async,
                          [&] { return context.execute(frozen.value()); });
  bool ready = false;
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    ready = gate.changed.wait_for(lock, std::chrono::seconds(5),
                                  [&] { return gate.published; });
  }
  auto restored =
      ready ? context.execute(frozen.value())
            : Result<ExecutionResult>(
                  Status{ErrorCode::Internal, "checkpoint was not published"});
  gate.unauthorized.store(true);
  auto denied = ready
                    ? context.execute(frozen.value())
                    : Result<ExecutionResult>(Status{ErrorCode::Internal, {}});
  gate.unauthorized.store(false);
  {
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.release = true;
  }
  gate.changed.notify_all();
  auto initial = first.get();
  PS_REQUIRE_OK(initial);
  PS_CHECK(ready);
  PS_REQUIRE_OK(restored);
  if (cached) {
    // The publisher was still blocked when this run reused the result created
    // by a checkpoint restore. Its replay omits the prefix Need entirely.
    PS_REQUIRE_OK(denied);
    PS_CHECK(denied.value().diagnostics.cache_hits >= 1);
  } else {
    PS_CHECK(denied.status().code == ErrorCode::InvalidArgument &&
             denied.status().reason == FailureReason::UnauthorizedRead);
  }
  PS_CHECK(starts[0] == 1 && starts[1] == (cached ? 1 : 2));
  for (const auto* run : {&initial, &restored, cached ? &denied : &restored}) {
    const auto& result = run->value();
    PS_CHECK(multi_result::number(result.results.at("sum")) == 14);
    auto record = result.dependencies.tile_record({1, 0, 0});
    PS_REQUIRE_OK(record);
    PS_CHECK(record.value() && record.value()->inputs.size() == 1 &&
             record.value()->inputs[0].tiles.size() == 2);
    for (TileIndex tile : {0, 1}) {
      ResourceVector<TileChange> changes{ResourceAllocator<TileChange>(root)};
      changes.push_back({PlanWorkflowInput{0}, 0,
                         TileSet::from_indices(root, {tile}).take_value(),
                         false});
      auto dirty = result.dependencies.dirty_units(changes);
      PS_REQUIRE_OK(dirty);
      PS_CHECK(dirty.value().size() == 2 &&
               (dirty.value()[0] == TileUnit{0, 0, tile}) &&
               (dirty.value()[1] == TileUnit{1, 0, 0}));
    }
  }
  auto rebound =
      context.freeze(compiled.value().plan,
                     {{multi_result::binding(root, "image", 11, schema)}});
  PS_REQUIRE_OK(rebound);
  auto changed = context.execute(rebound.value());
  PS_REQUIRE_OK(changed);
  PS_CHECK(multi_result::number(changed.value().results.at("sum")) == 22 &&
           starts[0] == 2 && starts[1] == (cached ? 2 : 3));
  return 0;
}
int empty_plans() {
  for (unsigned kind = 0; kind < 3; ++kind) {
    auto schema = multi_result::schema();
    if (kind) {
      schema.fields.push_back({"rows",
                               ElementType::Float64,
                               {kind == 1 ? ResultExtentKind::Fixed
                                          : ResultExtentKind::RuntimeCount,
                                1048577},
                               {}});
      if (kind == 1)
        schema.tensors.clear();
    }
    std::atomic<unsigned> producer_starts{0}, effect_starts{0};
    auto operations = std::make_shared<OperationRegistry>();
    for (bool effect : {false, true}) {
      OperationDefinition definition;
      definition.key = effect ? "test.empty_effect" : "test.empty_producer";
      definition.traits.outputs = {multi_result::output("value", schema)};
      definition.traits.outputs[0].region_rule = OperationRegionRule::Whole;
      definition.traits.side_effect_free = !effect;
      definition.traits.cacheable = !effect;
      definition.start_result = [&, effect](const auto&, const auto&) {
        ++(effect ? effect_starts : producer_starts);
        return Result<ResultContinuation>(
            Status{ErrorCode::OperationFailed, "unrequested producer"});
      };
      PS_CHECK(operations->register_operation(std::move(definition)).ok());
    }
    PS_CHECK(operations->freeze().ok());
    WorkflowDocument document;
    document.nodes = {{1, "test.empty_producer", {}, {}},
                      {2, "test.empty_effect", {}, {}}};
    document.outputs = {{"value", 1, "value"}};
    GraphContext graph(std::move(document));
    Compiler compiler(operations);
    PlanningOptions options;
    options.output_requests = {{"value", {}}};
    auto empty_plan = compiler.compile(graph, options);
    PS_REQUIRE_OK(empty_plan);
    ExecutionContext context(operations);
    const auto root = context.resource_budget().take_value();
    auto run = context.execute(empty_plan.value().plan);
    PS_REQUIRE_OK(run);
    const auto verify = [&](const ExecutionResult& result) {
      const auto& output = result.results.at("value");
      auto descriptor = output.descriptor().take_value();
      if (!descriptor.sealed() || !output.schema().same_schema(schema))
        return false;
      for (unsigned i = 0; i < schema.tensors.size(); ++i)
        if (!descriptor.tensor_coverage(i).empty())
          return false;
      for (unsigned i = 0; i < schema.fields.size(); ++i) {
        if (descriptor.rows(i) != 0)
          return false;
        if (output.prepare_read(descriptor, i, 0, 1).ok())
          return false;
      }
      return result.dependencies.valid() &&
             !result.dependencies.tile_record({0, 0, 0}).value();
    };
    PS_CHECK(verify(run.value()));
    auto full = compiler.compile(graph);
    PS_REQUIRE_OK(full);
    auto frozen = context.freeze(full.value().plan, {}).take_value();
    auto empty_frozen = frozen.for_tiles("value", {});
    PS_REQUIRE_OK(empty_frozen);
    auto pinned = context.execute(empty_frozen.value());
    PS_REQUIRE_OK(pinned);
    PS_CHECK(verify(pinned.value()) && producer_starts == 0 &&
             effect_starts == 0);
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
  }
  return 0;
}
struct CompoundProgram {
  bool partial;
  explicit CompoundProgram(bool partial) : partial(partial) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using multi_result::check;
    using multi_result::take;
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
      const double values[] = {10.0 * (slot + 1), 10.0 * (slot + 1) + 1};
      const auto count = slot == 1 && partial ? 1 : slot + 1;
      check(builder.publish_tensor(
          slot, Region({{0, count}}),
          ByteView(reinterpret_cast<const std::uint8_t*>(values),
                   count * sizeof(double))));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
struct MemberProgram {
  std::uint32_t slot;
  bool object, requested = false;
  MemberProgram(std::uint32_t slot, bool object) : slot(slot), object(object) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using multi_result::check;
    using multi_result::take;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      if (object) {
        need.tiles.push_back({0, true, false, {}});
      } else {
        auto selected = sdk::tile_need_for_samples(
            phase.resources, phase.query, 0, slot,
            take(Footprint::all({slot + 1U})), ((1) & 7U) == 0);
        if (!selected.ok())
          return Result<ResultProgramPoll>(selected.status());
        need.tiles.push_back(selected.take_value());
      }
      return Result<ResultProgramPoll>(std::move(need));
    }
    double value = 0;
    if (object) {
      if (!phase.tensors || phase.tensors->size() != 2)
        return Result<ResultProgramPoll>(Status{
            ErrorCode::OperationFailed, "Whole compound grant incomplete"});
      check(phase.read_tensor(0, slot, {slot}, &value, sizeof(value)));
    } else {
      check(phase.read_tensor(0, slot, {slot}, &value, sizeof(value)));
    }
    return LazyProgram(0, value).poll(phase);
  }
};
int compound_unit(bool partial) {
  auto schema = multi_result::schema();
  schema.tensors[0].key = "first";
  schema.tensors.push_back(schema.tensors[0]);
  schema.tensors[1].key = "second";
  schema.tensors[1].descriptor.shape = {2};
  auto operations = std::make_shared<OperationRegistry>();
  std::atomic<unsigned> starts{0};
  OperationDefinition producer;
  producer.key = "test.compound";
  producer.traits.outputs = {multi_result::output("value", schema)};
  producer.start_result = [&](const auto& query, const auto& allocator) {
    ++starts;
    if (query.tensor_slot != 0 || !query.tensor_outputs ||
        query.tensor_outputs.value() != Footprint::all({1}).take_value())
      return Result<ResultContinuation>(
          Status{ErrorCode::InvalidArgument, "noncanonical Whole query"});
    return ResultContinuation::make<CompoundProgram>(allocator, partial);
  };
  PS_CHECK(operations->register_operation(std::move(producer)).ok());
  WorkflowDocument document;
  document.nodes = {{1, "test.compound", {}, {}}};
  for (std::uint32_t port = 0; port < 3; ++port) {
    const auto key = "test.member." + std::to_string(port);
    OperationDefinition consumer;
    consumer.key = key;
    consumer.traits.input_count = 1;
    OperationPortConstraint input;
    input.kind = OperationPortKind::Result;
    input.result_schema_id = schema.id;
    input.result_schema_version = schema.version;
    consumer.traits.input_schema = {input};
    consumer.traits.outputs = {multi_result::output("value")};
    consumer.start_result = [port](const auto&, const auto& allocator) {
      return ResultContinuation::make<MemberProgram>(
          allocator, port == 0 ? 0U : 1U, port == 2);
    };
    PS_CHECK(operations->register_operation(std::move(consumer)).ok());
    document.nodes.push_back(
        {2 + port, key, {WorkflowNodeOutput{1, "value"}}, {}});
    document.outputs.push_back({std::to_string(port), 2 + port, "value"});
  }
  PS_CHECK(operations->freeze().ok());
  GraphContext graph(std::move(document));
  auto compiled = Compiler(operations).compile(graph);
  PS_REQUIRE_OK(compiled);
  ExecutionContext context(operations);
  auto result = context.execute(compiled.value().plan);
  if (partial) {
    PS_CHECK(result.status().code == ErrorCode::InvalidArgument &&
             result.status().detail.origin == FailureOrigin::Protocol &&
             starts == 1);
    return 0;
  }
  PS_REQUIRE_OK(result);
  PS_CHECK(starts == 1);
  PS_CHECK(multi_result::number(result.value().results.at("0")) == 10);
  PS_CHECK(multi_result::number(result.value().results.at("1")) == 21);
  PS_CHECK(multi_result::number(result.value().results.at("2")) == 21);
  return 0;
}
struct FieldMetadataProgram {
  bool whole = false, requested = false;
  explicit FieldMetadataProgram(bool whole) : whole(whole) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using multi_result::check;
    using multi_result::take;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back({0, whole, !whole, {}});
      return Result<ResultProgramPoll>(std::move(need));
    }
    double value = 0;
    if (whole) {
      if (phase.results.size() != 1)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::OperationFailed, "Whole field grant missing"});
      const auto& input = phase.results.at(0);
      auto descriptor = take(input.descriptor());
      auto read =
          take(input.prepare_read(descriptor, 0, 0, descriptor.rows(0)));
      auto bytes = take(read.load(4096));
      for (unsigned i = 0; i < 2; ++i) {
        double element;
        std::memcpy(&element, bytes->bytes().data() + i * 8, 8);
        value += element;
      }
    } else {
      if (!phase.results.empty() ||
          (phase.tensors && !phase.tensors->empty()) || !phase.descriptors ||
          phase.descriptors->size() != 1)
        return Result<ResultProgramPoll>(Status{
            ErrorCode::OperationFailed, "descriptor leaked payload grant"});
      value = phase.descriptors->at(0).rows(0);
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{}));

    check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&value), 8)));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
int invalid_envelope() {
  Fixture fixture(3, false, true, 2);
  GraphContext graph(fixture.document(true));
  auto compiled = Compiler(fixture.operations).compile(graph);
  PS_REQUIRE_OK(compiled);
  ExecutionContext context(fixture.operations);
  const auto root = context.resource_budget().take_value();
  auto control = multi_result::binding(root, "control", 0, fixture.scalar);
  auto result = context.execute(compiled.value().plan, {{control}});
  PS_CHECK(result.status().code == ErrorCode::InvalidArgument &&
           fixture.producer_calls == 0);
  return 0;
}
int field_grants() {
  for (bool whole : {false, true}) {
    auto operations = std::make_shared<OperationRegistry>();
    auto schema = multi_result::schema();
    schema.tensors.clear();
    schema.fields.push_back({"rows",
                             ElementType::Float64,
                             {ResultExtentKind::RuntimeCount, 0},
                             {}});
    OperationDefinition definition;
    definition.key = "test.field_facts";
    definition.traits.input_count = 1;
    OperationPortConstraint port;
    port.kind = OperationPortKind::Result;
    port.result_schema_id = schema.id;
    port.result_schema_version = schema.version;
    definition.traits.input_schema = {port};
    definition.traits.outputs = {multi_result::output("value")};
    definition.start_result = [whole](const auto&, const auto& allocator) {
      return ResultContinuation::make<FieldMetadataProgram>(allocator, whole);
    };
    PS_CHECK(operations->register_operation(std::move(definition)).ok());
    PS_CHECK(operations->freeze().ok());
    WorkflowDocument document;
    document.inputs = {multi_result::declaration(1, "input", schema)};
    document.nodes = {{2, "test.field_facts", {WorkflowInputReference{1}}, {}}};
    document.outputs = {{"value", 2, "value"}};
    GraphContext graph(std::move(document));
    auto compiled = Compiler(operations).compile(graph);
    PS_REQUIRE_OK(compiled);
    ExecutionContext context(operations);
    auto root = context.resource_budget().take_value();
    auto builder = ResultBuilder::start(root, schema, "fields").take_value();

    const double values[] = {10, 20};
    PS_CHECK(builder
                 .append(0, 2,
                         ByteView(reinterpret_cast<const std::uint8_t*>(values),
                                  sizeof(values)))
                 .ok());
    PS_CHECK(builder.publish(0, 2).ok());
    auto input = builder.seal();
    PS_REQUIRE_OK(input);
    auto result = context.execute(compiled.value().plan,
                                  {{{"input", input.take_value()}}});
    PS_REQUIRE_OK(result);
    PS_CHECK(multi_result::number(result.value().results.at("value")) ==
             (whole ? 30 : 2));
  }
  return 0;
}
struct StaticMetadataProgram {
  unsigned denied;
  bool requested = false;
  explicit StaticMetadataProgram(unsigned denied) : denied(denied) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using multi_result::check;
    using multi_result::take;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back({0, false, true, {}});
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (!phase.results.empty() || !phase.descriptors ||
        phase.descriptors->size() != 1)
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed, "static descriptor grants"});
    const auto& facts = phase.descriptors->at(0);
    if (facts.object_id() || facts.revision() || facts.sealed())
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed, "static facts contain progress"});
    double value = facts.field_count() ? facts.rows(0) : 1;
    if (!phase.query.inputs[0].result_schema->tensors.empty()) {
      const auto& input = phase.tensors->at({0, 0});
      if (input.spec().descriptor.element_type != ElementType::Float64 ||
          !input.coverage().empty() || input.object_id())
        return Result<ResultProgramPoll>(
            Status{ErrorCode::OperationFailed, "static tensor capability"});
      if (denied == 1)
        static_cast<void>(input.read({0}, &value, 8));
      if (denied == 2)
        static_cast<void>(input.acquire(Region::whole({1})));
    }
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));

    check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&value), 8)));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
int static_descriptors() {
  for (unsigned kind : {0U, 1U})
    for (unsigned denied = 0; denied < (kind ? 1U : 3U); ++denied) {
      auto registry = std::make_shared<OperationRegistry>();
      auto schema = multi_result::schema();
      if (kind) {
        schema.tensors.clear();
        schema.fields.push_back(
            {"rows", ElementType::Float64, {ResultExtentKind::Fixed, 2}, {}});
      }
      unsigned starts = 0;
      OperationDefinition producer;
      producer.key = "test.static_failure";
      producer.traits.outputs = {multi_result::output("value", schema)};
      producer.traits.outputs[0].region_rule = OperationRegionRule::Whole;
      producer.start_result = [&](const auto&, const auto&) {
        ++starts;
        return Result<ResultContinuation>(Status{
            ErrorCode::OperationFailed, "payload must remain unrequested"});
      };
      PS_CHECK(registry->register_operation(std::move(producer)).ok());
      OperationDefinition consumer;
      consumer.key = "test.static_metadata";
      consumer.traits.input_count = 1;
      OperationPortConstraint port;
      port.kind = OperationPortKind::Result;
      port.result_schema_id = schema.id;
      port.result_schema_version = schema.version;
      consumer.traits.input_schema = {port};
      consumer.traits.outputs = {multi_result::output("value")};
      consumer.start_result = [denied](const auto&, const auto& allocator) {
        return ResultContinuation::make<StaticMetadataProgram>(allocator,
                                                               denied);
      };
      PS_CHECK(registry->register_operation(std::move(consumer)).ok());
      PS_CHECK(registry->freeze().ok());
      WorkflowDocument document;
      document.nodes = {
          {1, "test.static_failure", {}, {}},
          {2, "test.static_metadata", {WorkflowNodeOutput{1, "value"}}, {}}};
      document.outputs = {{"value", 2, "value"}};
      GraphContext graph(std::move(document));
      auto compiled = Compiler(registry).compile(graph);
      PS_REQUIRE_OK(compiled);
      ExecutionContext context(registry);
      auto run = context.execute(compiled.value().plan);
      PS_CHECK(starts == 0);
      if (denied) {
        PS_CHECK(run.status().code == ErrorCode::InvalidArgument &&
                 run.status().detail.origin == FailureOrigin::Protocol);
      } else {
        PS_REQUIRE_OK(run);
        PS_CHECK(multi_result::number(run.value().results.at("value")) ==
                 (kind ? 2 : 1));
      }
    }
  return 0;
}
struct OutOfGridProgram {
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ResultProgramNeed need;
    need.tiles.push_back({0, true, false, {}});
    auto tiles = TileSet::from_indices(phase.resources, {1});
    if (!tiles.ok())
      return Result<ResultProgramPoll>(tiles.status());
    need.tiles.push_back({1, false, false, tiles.take_value()});
    return Result<ResultProgramPoll>(std::move(need));
  }
};
int invalid_tile_bounds() {
  auto registry = std::make_shared<OperationRegistry>();
  auto schema = multi_result::schema();
  schema.tensors[0].descriptor.shape = {2, 2};
  schema.tensors[0].layout.spatial = true;
  schema.tensors[0].layout.channel_axis.reset();
  unsigned starts = 0;
  OperationDefinition producer;
  producer.key = "test.grid_failure";
  producer.traits.outputs = {multi_result::output("value", schema)};
  producer.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  producer.start_result = [&](const auto&, const auto&) {
    ++starts;
    return Result<ResultContinuation>(
        Status{ErrorCode::OperationFailed, "must validate all Needs first"});
  };
  PS_CHECK(registry->register_operation(std::move(producer)).ok());
  OperationDefinition consumer;
  consumer.key = "test.grid_need";
  consumer.traits.input_count = 2;
  OperationPortConstraint port;
  port.kind = OperationPortKind::Result;
  port.result_schema_id = schema.id;
  port.result_schema_version = schema.version;
  consumer.traits.input_schema = {port, port};
  consumer.traits.outputs = {multi_result::output("value")};
  consumer.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<OutOfGridProgram>(allocator);
  };
  PS_CHECK(registry->register_operation(std::move(consumer)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.nodes = {
      {1, "test.grid_failure", {}, {}},
      {2,
       "test.grid_need",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{1, "value"}},
       {}}};
  document.outputs = {{"value", 2, "value"}};
  GraphContext graph(std::move(document));
  auto compiled = Compiler(registry).compile(graph);
  PS_REQUIRE_OK(compiled);
  ExecutionContext context(registry);
  auto run = context.execute(compiled.value().plan);
  PS_CHECK(run.status().code == ErrorCode::InvalidArgument &&
           run.status().detail.origin == FailureOrigin::Protocol &&
           starts == 0);
  return 0;
}
struct MaskGrantProgram {
  bool descriptor_only, requested = false;
  explicit MaskGrantProgram(bool descriptor_only)
      : descriptor_only(descriptor_only) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using multi_result::check;
    using multi_result::take;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back({0, false, descriptor_only,
                            descriptor_only ? TileSet{}
                                            : take(TileSet::from_indices(
                                                  phase.resources, {0}))});
      return Result<ResultProgramPoll>(std::move(need));
    }
    float first = .5F;
    if (!descriptor_only)
      check(phase.read_tensor(0, 0, {0, 0}, &first, sizeof(first)));
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    const double answer = first;
    check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&answer),
                 sizeof(answer))));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
int typed_tile_grants() {
  using multi_result::check;
  using multi_result::take;
  auto schema = multi_result::schema(ElementType::Float32, {1, 4});
  schema.id = "test.mask_grant";
  schema.tensors[0].layout.spatial = true;
  schema.tensors[0].layout.channel_axis.reset();
  for (bool descriptor_only : {false, true}) {
    auto registry = std::make_shared<OperationRegistry>();
    OperationDefinition definition;
    ValidationDefinition validator;
    validator.identity = {"test.finite", 1, "float32/1"};
    validator.prepare = [](const ValidationPreparation&) {
      return Result<std::shared_ptr<const void>>(std::shared_ptr<const void>{});
    };
    validator.validate = [](const void*, const ValidationContext& context) {
      const auto* coverage = context.tensor_coverage(0);
      if (!coverage)
        return Status::success();
      for (const auto& box : coverage->boxes()) {
        auto window = context.acquire_tensor(0, box);
        if (!window.ok())
          return window.status();
        auto status =
            take(Footprint::from_regions(coverage->shape(), {box}))
                .visit(
                    [&](const auto& at) {
                      auto charged = context.consume(1);
                      if (!charged.ok())
                        return charged;
                      auto run = window.value().row_run(at);
                      if (!run.ok())
                        return run.status();
                      float sample;
                      std::memcpy(&sample, run.value().data, sizeof(sample));
                      return std::isfinite(sample)
                                 ? Status::success()
                                 : Status{ErrorCode::OperationFailed,
                                          "nonfinite sample"};
                    },
                    UINT64_MAX, context.cancellation);
        if (!status.ok())
          return status;
      }
      return Status::success();
    };
    check(registry->register_validator(validator));
    definition.key = "test.mask_grant";
    definition.traits.input_count = 1;
    OperationPortConstraint port;
    port.kind = OperationPortKind::Result;
    port.result_schema_id = schema.id;
    port.result_schema_version = schema.version;
    port.validators = {validator.identity};
    definition.traits.input_schema = {port};
    definition.traits.outputs = {multi_result::output("value")};
    definition.traits.outputs[0].region_rule = OperationRegionRule::Whole;
    definition.start_result = [descriptor_only](const auto&,
                                                const auto& allocator) {
      return ResultContinuation::make<MaskGrantProgram>(allocator,
                                                        descriptor_only);
    };
    check(registry->register_operation(std::move(definition)));
    check(registry->freeze());
    WorkflowDocument document;
    document.inputs = {multi_result::declaration(1, "mask", schema)};
    document.nodes = {{1, "test.mask_grant", {WorkflowInputReference{1}}, {}}};
    document.outputs = {{"value", 1, "value"}};
    GraphContext graph(document);
    PlanningOptions planning;
    planning.tile_extent = 2;
    auto plan = take(Compiler(registry).compile(graph, planning)).plan;
    ExecutionContext context(registry);
    const auto root = take(context.resource_budget());
    for (unsigned invalid_sample : {1U, 3U}) {
      const auto values =
          std::vector<float>{.5F, invalid_sample == 1 ? NAN : .25F, .75F,
                             invalid_sample == 3 ? NAN : 1};
      auto builder = take(ResultBuilder::start(root, schema, "mask.binding"));
      check(builder.publish_tensor(
          0, Region::whole({1, 4}),
          ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                   values.size() * sizeof(float))));
      ExecutionBinding binding;
      binding.name = "mask";
      binding.result = take(builder.seal());
      auto result = context.execute(plan, ExecutionBindings{{binding}});
      if (!descriptor_only && invalid_sample == 1) {
        PS_CHECK(!result.ok() &&
                 result.status().code == ErrorCode::InvalidArgument &&
                 result.status().detail.input_id == 1);
      } else {
        PS_REQUIRE_OK(result);
        PS_CHECK(multi_result::number(result.value().results.at("value")) ==
                 .5);
      }
    }
  }
  return 0;
}
struct ReportedFailureProgram {
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    phase.record_failure(Status::success());
    auto reporting = phase;
    reporting.failure_observer = [](const Status&) {
      throw std::runtime_error("observer failure");
    };
    reporting.record_failure({ErrorCode::OperationFailed,
                              "numeric domain failure",
                              FailureReason::InvalidDomain,
                              {FailureOrigin::Domain, FailureScope::Run}});
    reporting.record_failure({ErrorCode::BackendUnavailable, {}});
    return Result<ResultProgramPoll>(Status{ErrorCode::Cancelled, {}});
  }
};
int reported_failures() {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition definition;
  definition.key = "test.reported_failure";
  definition.traits.input_count = 0;
  definition.traits.input_schema.clear();
  definition.traits.outputs = {multi_result::output("value")};
  definition.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<ReportedFailureProgram>(allocator);
  };
  multi_result::check(registry->register_operation(std::move(definition)));
  multi_result::check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "test.reported_failure", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto plan = multi_result::take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry);
  auto result = context.execute(plan);
  PS_CHECK(!result.ok() && result.status().code == ErrorCode::OperationFailed);
  PS_CHECK(result.status().reason == FailureReason::InvalidDomain);
  PS_CHECK(result.status().message == "numeric domain failure");
  PS_CHECK(result.status().detail.origin == FailureOrigin::Domain &&
           result.status().detail.scope == FailureScope::Run);
  return 0;
}
}  // namespace
int main() {
  return run_cases() || resolved_whole() || planned_tiles() ||
         atomic_unit_publications() || repeated_tile_needs() ||
         checkpoint_tile_dependencies() || checkpoint_tile_dependencies(true) ||
         empty_plans() || compound_unit(false) || compound_unit(true) ||
         field_grants() || invalid_envelope() || static_descriptors() ||
         invalid_tile_bounds() || typed_tile_grants() || reported_failures();
}
