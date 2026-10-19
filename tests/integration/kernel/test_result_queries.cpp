#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "photospider/plugin/whole_stage.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
SchemaTemplate source_schema(unsigned width) {
  auto schema = multi_result::schema(ElementType::Float64, {1, width});
  schema.tensors[0].layout.spatial = true;
  schema.tensors[0].layout.channel_axis.reset();
  return schema;
}
TileNeed tile_need(const ResultProgramPhase& phase, TileIndex tile) {
  return {0, false, false,
          take(TileSet::from_indices(phase.resources, {tile}))};
}
ResourceVector<TileUnit> dirty(const ExecutionDependencies& dependencies,
                               const ResourceBudget& root, TileIndex tile) {
  ResourceVector<TileChange> changes{ResourceAllocator<TileChange>(root)};
  changes.push_back({PlanWorkflowInput{0}, 0, 0,
                     take(TileSet::from_indices(root, {tile})), false});
  return take(dependencies.dirty_units(changes));
}
ExecutionPlan compile(const std::shared_ptr<OperationRegistry>& registry,
                      GraphContext& graph) {
  PlanningOptions planning;
  planning.tile_extent = 1;
  return take(Compiler(registry).compile(graph, planning)).plan.value();
}
ResultProgramPoll publication(const ResultProgramPhase& phase, double value) {
  auto builder = take(ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {}, phase.query.tile_extent));
  const auto& tensor = phase.query.output.result_schema->tensors[0];
  auto coverage = phase.query.tensor_outputs.value_or(
      take(Footprint::all(tensor.sample_shape())));
  for (const auto& box : coverage.boxes()) {
    if (take(box.element_count()) != 1)
      throw std::runtime_error("source query is not one sample");
    auto storage = take(phase.resources.allocator().allocate(sizeof(value)));
    std::memcpy(storage.data(), &value, sizeof(value));
    StridedLayout layout;
    layout.byte_strides.assign(box.rank(), sizeof(value));
    for (const auto& dimension : box.dimensions())
      layout.origin.push_back(dimension.offset);
    check(builder.publish_tensor(0, box, std::move(layout),
                                 std::move(storage).freeze()));
  }
  return ResultPublication{take(builder.seal()), true};
}
struct Source {
  std::shared_ptr<std::atomic<unsigned>> calls;
  std::function<void(std::uint64_t)> hook;
  Source(std::shared_ptr<std::atomic<unsigned>> count,
         std::function<void(std::uint64_t)> callback)
      : calls(std::move(count)), hook(std::move(callback)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++*calls;
    auto at = phase.query.tensor_outputs->boxes()[0].dimensions()[1].offset;
    if (hook)
      hook(at);
    return Result<ResultProgramPoll>(publication(phase, 10 + at));
  }
};
OperationDefinition source_definition(
    unsigned width, const std::shared_ptr<std::atomic<unsigned>>& calls,
    std::function<void(std::uint64_t)> hook = {}) {
  OperationDefinition definition;
  definition.key = "queries.source";
  definition.traits.input_count = 0;
  definition.traits.input_schema.clear();
  definition.traits.outputs = {
      multi_result::output("value", source_schema(width))};
  definition.start_result = [calls, hook](const auto&, const auto& allocator) {
    return ResultContinuation::make<Source>(allocator, calls, hook);
  };
  return definition;
}
struct SinglePair {
  unsigned stage = 0;
  bool metadata;
  explicit SinglePair(bool metadata) : metadata(metadata) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto at = phase.query.output_index;
    if (stage < (metadata ? 2U : 1U)) {
      ResultProgramNeed need;
      need.tiles.push_back(metadata && stage == 0 ? TileNeed{0, 0, true, {}}
                                                  : tile_need(phase, at));
      ++stage;
      return Result<ResultProgramPoll>(std::move(need));
    }
    double value = 0;
    check(phase.tensors->at({0, 0}).read({0, at}, &value, sizeof(value)));
    return Result<ResultProgramPoll>(publication(phase, value));
  }
};
struct Sum {
  unsigned width, next = 0;
  double total = 0;
  explicit Sum(unsigned width) : width(width) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (next) {
      double value = 0;
      check(
          phase.tensors->at({0, 0}).read({0, next - 1}, &value, sizeof(value)));
      total += value;
    }
    if (next < width) {
      ResultProgramNeed need;
      need.tiles.push_back(tile_need(phase, next++));
      return Result<ResultProgramPoll>(std::move(need));
    }
    return Result<ResultProgramPoll>(publication(phase, total));
  }
};
OperationDefinition consumer_definition(bool pair, unsigned width,
                                        bool metadata = false) {
  OperationDefinition definition;
  definition.key = "queries.consumer";
  definition.traits.input_count = 1;
  definition.traits.input_schema.resize(1);

  definition.traits.input_schema[0].result_schema_id = "test.multi_output";
  definition.traits.input_schema[0].result_schema_version = 1;
  definition.traits.outputs = {multi_result::output(pair ? "left" : "value")};
  definition.traits.outputs[0].maximum_dependency_stages = width + 1;
  if (pair) {
    definition.traits.outputs.push_back(multi_result::output("right"));
    definition.traits.outputs[1].maximum_dependency_stages = width + 1;
    definition.map_dependencies = [](const ResultStaticQuery& query) {
      ResultStaticMapping mapping;
      mapping.inputs.push_back(
          {0, 0, false,
           take(TileSet::range(ResourceBudget{}, query.output_index,
                               query.output_index + 1))});
      return Result<ResultStaticMapping>(std::move(mapping));
    };
  }
  definition.start_result = [pair, width, metadata](const auto&,
                                                    const auto& allocator) {
    return pair ? ResultContinuation::make<SinglePair>(allocator, metadata)
                : ResultContinuation::make<Sum>(allocator, width);
  };
  return definition;
}
int workflow(bool pair) {
  const unsigned width = pair ? 2 : 64;
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(source_definition(width, calls)));
  check(registry->register_operation(consumer_definition(pair, width)));
  check(registry->freeze());
  WorkflowDocument doc;
  doc.nodes = {{10, "queries.source", {}, {}},
               {20, "queries.consumer", {WorkflowNodeOutput{10, "value"}}, {}}};
  doc.outputs = pair ? std::vector<WorkflowOutput>{{"left", 20, "left"},
                                                   {"right", 20, "right"}}
                     : std::vector<WorkflowOutput>{{"total", 20, "value"}};
  GraphContext graph(doc);
  auto plan = compile(registry, graph);
  ExecutionContext context(registry, {2, false, 16, 1024 * 1024, 0});
  auto root = take(context.resource_budget());
  std::vector<WeakResultRef> sources;
  unsigned notices = 0, previous_live = 0;
  ExecutionOptions options;
  options.maximum_dependency_work = 10000000;
  options.dependencies.sets.maximum_work = 10000000;
  options.result_publication = [&](ValueRef ref, const ResultRef& object) {
    if (ref.node_id != 10)
      return Status::success();
    ++notices;
    unsigned alive = 0;
    for (const auto& old : sources)
      alive += old.lock().valid();
    previous_live = std::max(previous_live, alive);
    sources.push_back(object.weak());
    return Status::success();
  };
  auto result = context.execute(plan, {}, {}, options);
  if (!result.ok())
    std::cerr << "query workflow: " << result.status().message << '\n';
  PS_CHECK(result.ok() && *calls == width && notices == width);
  if (pair) {
    PS_CHECK(multi_result::number(result.value().results.at("left")) == 10);
    PS_CHECK(multi_result::number(result.value().results.at("right")) == 11);
  } else {
    PS_CHECK(multi_result::number(result.value().results.at("total")) ==
             width * 10 + width * (width - 1) / 2);
    PS_CHECK(previous_live == width - 1);
    for (const auto& old : sources)
      PS_CHECK(!old.lock().valid());
  }
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
struct ControlledSource {
  bool requested = false;
  std::shared_ptr<std::atomic<unsigned>> calls;
  explicit ControlledSource(std::shared_ptr<std::atomic<unsigned>> count)
      : calls(std::move(count)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto at =
        phase.query.tensor_outputs->boxes()[0].dimensions()[1].offset;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back(tile_need(phase, at));
      return Result<ResultProgramPoll>(std::move(need));
    }
    ++*calls;
    double control = 0;
    check(phase.tensors->at({0, 0}).read({0, at}, &control, sizeof(control)));
    return Result<ResultProgramPoll>(
        publication(phase, (control > 0 ? 10 : 20) + at));
  }
};
OperationDefinition controlled_definition(
    const std::shared_ptr<std::atomic<unsigned>>& calls) {
  auto source = source_definition(2, calls);
  source.traits.input_count = 1;
  source.traits.input_schema.resize(1);

  source.traits.input_schema[0].result_schema_id = "test.multi_output";
  source.traits.input_schema[0].result_schema_version = 1;
  source.map_dependencies = [](const ResultStaticQuery& query) {
    ResultStaticMapping mapping;
    mapping.inputs.push_back(
        {0, 0, false,
         take(TileSet::range(ResourceBudget{}, query.tile_unit,
                             query.tile_unit + 1))});
    return Result<ResultStaticMapping>(std::move(mapping));
  };
  source.start_result = [calls](const auto&, const auto& allocator) {
    return ResultContinuation::make<ControlledSource>(allocator, calls);
  };
  return source;
}
int query_dependencies(bool metadata) {
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(controlled_definition(calls)));
  check(registry->register_operation(consumer_definition(true, 2, metadata)));
  check(registry->freeze());
  WorkflowDocument doc;
  const auto schema = source_schema(2);
  doc.inputs = {multi_result::declaration(1, "control", schema)};
  doc.nodes = {{10, "queries.source", {WorkflowInputReference{1}}, {}},
               {20, "queries.consumer", {WorkflowNodeOutput{10, "value"}}, {}}};
  doc.outputs = {{"left", 20, "left"}, {"right", 20, "right"}};
  GraphContext graph(doc);
  auto plan = compile(registry, graph);
  ExecutionContextConfig config{1, false, 16, 1024 * 1024, 1048576};
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  auto run_plan = plan;
  ExecutionBindings run_bindings =
      ExecutionBindings{{multi_result::binding(root, "control", 1, schema)}};
  const TileRequest one{{{0, take(TileSet::range(root, 0, 1))}}};
  const DemandQuery wanted{{"left", one}, {"right", one}};
  const auto verify = [&](const DemandResult& result) {
    for (unsigned at = 0; at < 2; ++at) {
      auto units = dirty(result.dependencies, root, at);
      check(
          units.size() == 3 && units[0] == TileUnit{0, 0, 0, at} &&
                  units[1] == TileUnit{at + 1, at, 0} &&
                  units[2] ==
                      TileUnit{plan.output(at ? "right" : "left").record_index,
                               0, 0}
              ? Status::success()
              : Status{ErrorCode::OperationFailed, "tile ancestry mismatch"});
      auto record = take(result.dependencies.tile_record({at + 1, at, 0}));
      check(record && record->inputs.size() == 1 &&
                    record->inputs[0].tiles.size() == 1 &&
                    record->inputs[0].tiles.contains(at)
                ? Status::success()
                : Status{ErrorCode::OperationFailed, "selected tile record"});
    }
    check(multi_result::number(result.results.at("left")) == 10 &&
                  multi_result::number(result.results.at("right")) == 11
              ? Status::success()
              : Status{ErrorCode::OperationFailed, "query values changed"});
  };
  auto cold = take(context.execute_fragments(run_plan, run_bindings, wanted));
  verify(cold);
  PS_CHECK(*calls == 2);
  auto equivalent_plan = plan;
  ExecutionBindings equivalent_bindings =
      ExecutionBindings{{multi_result::binding(root, "control", 1, schema)}};
  auto warm = take(
      context.execute_fragments(equivalent_plan, equivalent_bindings, wanted));
  verify(warm);
  PS_CHECK(*calls == 2 && warm.diagnostics.cache_hits >= 2 &&
           root.statistics().live[ResourceKind::Queue] == 0);
  auto builder = take(ResultBuilder::start(root, schema, "changed.control"));

  const std::array<double, 2> values{1, -1};
  check(builder.publish_tensor(
      0, Region::whole({1, 2}),
      {reinterpret_cast<const std::uint8_t*>(values.data()), sizeof(values)}));
  ExecutionBinding binding;
  binding.name = "control";
  binding.result = take(builder.seal());
  auto changed_plan = plan;
  ExecutionBindings changed_bindings = ExecutionBindings{{binding}};
  auto partial =
      take(context.execute_fragments(changed_plan, changed_bindings, wanted));
  PS_CHECK(multi_result::number(partial.results.at("left")) == 10 &&
           multi_result::number(partial.results.at("right")) == 21 &&
           *calls == 3 && partial.diagnostics.cache_hits >= 1);
  for (unsigned at = 0; at < 2; ++at) {
    auto units = dirty(partial.dependencies, root, at);
    PS_CHECK(units.size() == 3 && (units[0] == TileUnit{0, 0, 0, at}) &&
             (units[1] == TileUnit{at + 1, at, 0}) &&
             (units[2] ==
              TileUnit{plan.output(at ? "right" : "left").record_index, 0, 0}));
  }
  return 0;
}
struct Partial {
  unsigned stage = 0;
  std::optional<ResultBuilder> builder;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto at = stage / 2;
    if (!(stage++ % 2)) {
      ResultProgramNeed need;
      need.tiles.push_back(tile_need(phase, at));
      return Result<ResultProgramPoll>(std::move(need));
    }
    double value = 0;
    check(phase.tensors->at({0, 0}).read({0, at}, &value, sizeof(value)));
    if (!builder) {
      builder.emplace(take(ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key)));
    }
    const Region point({{at, 1}});
    check(builder->publish_tensor(
        0, point,
        {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)}));
    if (stage == 2) {
      ++stage;
      ResultProgramNeed need;
      need.tiles.push_back(tile_need(phase, 1));
      return Result<ResultProgramPoll>(std::move(need));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder->seal()), true});
  }
};
int shared_publication_dependencies() {
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(controlled_definition(calls)));
  auto consumer = consumer_definition(false, 2);
  auto partial_schema = multi_result::schema(ElementType::Float64, {2});
  consumer.traits.outputs = {multi_result::output("value", partial_schema)};
  consumer.traits.outputs[0].maximum_dependency_stages = 4;
  consumer.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<Partial>(allocator);
  };
  check(registry->register_operation(
      sdk::with_whole_stage(std::move(consumer), 100000)));
  check(registry->freeze());
  WorkflowDocument doc;
  const auto schema = source_schema(2);
  doc.inputs = {multi_result::declaration(1, "control", schema)};
  doc.nodes = {{10, "queries.source", {WorkflowInputReference{1}}, {}},
               {20, "queries.consumer", {WorkflowNodeOutput{10, "value"}}, {}}};
  doc.outputs = {{"value", 20, "value"}};
  GraphContext graph(doc);
  auto plan = compile(registry, graph);
  ExecutionContext context(registry, {2, false, 16, 1024 * 1024, 0});
  auto root = take(context.resource_budget());
  unsigned notices = 0;
  ResultRef first;
  ExecutionOptions options;
  options.result_publication = [&](ValueRef ref, const ResultRef& object) {
    if (ref.node_id == 20 && ++notices == 1)
      first = object;
    return Status::success();
  };
  auto run_plan = plan;
  ExecutionBindings run_bindings =
      ExecutionBindings{{multi_result::binding(root, "control", 1, schema)}};
  auto result = take(context.execute_fragments(
      run_plan, run_bindings,
      {{"value", {{{0, take(TileSet::range(root, 0, 1))}}}}}, {}, options));
  // The requested tile is exposed only after its shared producer completes.
  PS_CHECK(notices == 1 && *calls == 2);
  PS_CHECK(first.descriptor(false).value().tensor_coverage(0) ==
           take(Footprint::from_regions({2}, {Region({{0, 1}})})));
  PS_CHECK(first.descriptor(false).value().tensor_coverage(0) ==
           take(result.results.at("value").descriptor()).tensor_coverage(0));
  for (unsigned at = 0; at < 2; ++at) {
    auto units = dirty(result.dependencies, root, at);
    PS_CHECK(units.size() == 3 && (units[0] == TileUnit{0, 0, 0, at}) &&
             (units[1] == TileUnit{1, 0, 0}) &&
             (units[2] == TileUnit{plan.output("value").record_index, 0, 0}));
    if (at == 0)
      PS_CHECK(multi_result::number(result.results.at("value"), {at}) == 10 &&
               multi_result::number(first, {at}) == 10);
  }
  double unavailable = 0;
  PS_CHECK(!first.read_tensor(take(first.descriptor()), 0, {1}, &unavailable, 8)
                .ok());
  const auto& returned = result.results.at("value");
  PS_CHECK(
      !returned
           .read_tensor(take(returned.descriptor()), 0, {1}, &unavailable, 8)
           .ok());
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
struct Mixed {
  unsigned stage = 0;
  double value = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (stage < 2) {
      if (stage == 1)
        check(phase.tensors->at({0, 0}).read({0, 0}, &value, sizeof(value)));
      ResultProgramNeed need;
      need.tiles.push_back(tile_need(phase, stage));
      ++stage;
      return Result<ResultProgramPoll>(std::move(need));
    }
    return Result<ResultProgramPoll>(publication(phase, value));
  }
};
int mixed_target_dependencies() {
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(controlled_definition(calls)));
  auto consumer = consumer_definition(false, 2);
  consumer.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<Mixed>(allocator);
  };
  check(registry->register_operation(std::move(consumer)));
  check(registry->freeze());
  WorkflowDocument doc;
  const auto schema = source_schema(2);
  doc.inputs = {multi_result::declaration(1, "control", schema)};
  doc.nodes = {{10, "queries.source", {WorkflowInputReference{1}}, {}},
               {20, "queries.consumer", {WorkflowNodeOutput{10, "value"}}, {}}};
  doc.outputs = {{"value", 20, "value"}};
  GraphContext graph(doc);
  auto plan = compile(registry, graph);
  ExecutionContext context(registry, {1, false, 16, 1024 * 1024, 1048576});
  auto root = take(context.resource_budget());
  const auto run = [&] {
    return take(context.execute_fragments(
        plan, {{multi_result::binding(root, "control", 1, schema)}},
        {{"value", {{{0, take(TileSet::range(root, 0, 1))}}}}}));
  };
  const auto verify = [&](const DemandResult& result) {
    for (unsigned at = 0; at < 2; ++at) {
      auto units = dirty(result.dependencies, root, at);
      check(units.size() == 3 && units[0] == TileUnit{0, 0, 0, at} &&
                    units[1] == TileUnit{1, 0, 0} &&
                    units[2] ==
                        TileUnit{plan.output("value").record_index, 0, 0}
                ? Status::success()
                : Status{ErrorCode::OperationFailed, "staged Need ancestry"});
    }
    check(
        multi_result::number(result.results.at("value")) == 10
            ? Status::success()
            : Status{ErrorCode::OperationFailed, "mixed target value changed"});
  };
  auto cold = run();
  verify(cold);
  auto warm = run();
  verify(warm);
  PS_CHECK(*calls == 2 && warm.diagnostics.cache_hits >= 1 &&
           root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned entered = 0;
  bool released = false, timed_out = false;
  void hold() {
    std::unique_lock<std::mutex> lock(mutex);
    ++entered;
    changed.notify_all();
    if (!changed.wait_for(lock, std::chrono::seconds(5),
                          [&] { return released; }))
      timed_out = true;
  }
  bool wait(unsigned count = 1) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(5),
                            [&] { return entered >= count; });
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
};
int named_tile_aliases(unsigned width, bool reverse) {
  Gate gate;
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(
      source_definition(width, calls, [&](std::uint64_t) { gate.hold(); })));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{10, "queries.source", {}, {}}};
  document.outputs = {{"a", 10, "value"},
                      {"b", 10, "value"},
                      {"c", 10, "value"}};
  GraphContext graph(document);
  auto plan = compile(registry, graph);
  ExecutionContext context(registry, {2, false, 16, 1024 * 1024, 0});
  auto root = take(context.resource_budget());
  const TileRequest low{{{0, take(TileSet::range(root, 0, width / 2))}}};
  const TileRequest high{{{0, take(TileSet::range(root, width / 2, width))}}};
  const DemandQuery query{{"a", reverse ? high : low},
                          {"b", reverse ? low : high},
                          {"c", reverse ? high : low}};
  auto run_plan = plan;
  ExecutionBindings run_bindings = {};
  auto pending = std::async(std::launch::async, [&] {
    ExecutionOptions options;
    options.maximum_parallelism = 2;
    return context.execute_fragments(run_plan, run_bindings, query, {},
                                     options);
  });
  const bool entered = gate.wait();
  gate.release();
  auto result = pending.get();
  PS_REQUIRE_OK(result);
  PS_CHECK(entered && !gate.timed_out && *calls == width);
  for (const auto& named : query) {
    const auto& object = result.value().results.at(named.first.c_str());
    const auto facts = take(object.descriptor());
    for (unsigned at = 0; at < width; ++at) {
      double value = 0;
      auto read = object.read_tensor(facts, 0, {0, at}, &value, sizeof(value));
      PS_CHECK(named.second.members.at(0).contains(at)
                   ? read.ok() && value == 10 + at
                   : !read.ok());
    }
  }
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
int upstream_run_cancellation() {
  Gate gate;
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(
      source_definition(2, calls, [&](std::uint64_t at) {
        if (at == 1)
          gate.hold();
      })));
  check(registry->register_operation(consumer_definition(true, 2)));
  check(registry->freeze());
  WorkflowDocument doc;
  doc.nodes = {{10, "queries.source", {}, {}},
               {20, "queries.consumer", {WorkflowNodeOutput{10, "value"}}, {}}};
  doc.outputs = {{"left", 20, "left"}, {"right", 20, "right"}};
  GraphContext graph(doc);
  auto plan = compile(registry, graph);
  ExecutionContext context(registry, {2, false, 16, 1024 * 1024, 0});
  auto root = take(context.resource_budget());
  auto run_plan = plan;
  ExecutionBindings run_bindings = {};
  CancellationSource cancellation;
  auto first = std::async(std::launch::async, [&] {
    return context.execute(run_plan, run_bindings, cancellation.token());
  });
  const bool entered = gate.wait();
  auto peer = std::async(std::launch::async, [&] {
    return context.execute(run_plan, run_bindings);
  });
  const bool independent = gate.wait(2);
  cancellation.cancel();
  const bool drains = first.wait_for(std::chrono::milliseconds(20)) !=
                      std::future_status::ready;
  gate.release();
  auto cancelled = first.get();
  auto healthy = peer.get();
  PS_CHECK(entered && independent && drains && !gate.timed_out);
  PS_CHECK(cancelled.status().code == ErrorCode::Cancelled && healthy.ok());
  PS_CHECK(multi_result::number(healthy.value().results.at("left")) == 10 &&
           multi_result::number(healthy.value().results.at("right")) == 11);
  PS_CHECK(*calls == 4 && root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
}  // namespace
int main() try {
  PS_CHECK(workflow(true) == 0);
  PS_CHECK(workflow(false) == 0);
  PS_CHECK(named_tile_aliases(2, true) == 0);
  PS_CHECK(named_tile_aliases(8, false) == 0);
  PS_CHECK(upstream_run_cancellation() == 0);
  PS_CHECK(query_dependencies(false) == 0);
  PS_CHECK(query_dependencies(true) == 0);
  PS_CHECK(shared_publication_dependencies() == 0);
  PS_CHECK(mixed_target_dependencies() == 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
