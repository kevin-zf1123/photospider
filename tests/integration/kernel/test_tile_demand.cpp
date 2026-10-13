#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool armed = false, entered = false, release = false;
  std::atomic<bool> fail{false};
};
struct Program {
  explicit Program(Gate* gate) : gate(gate) {}
  Gate* gate;
  unsigned stage = 0;
  TileIndex source = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using multi_result::check;
    using multi_result::take;
    if (!stage++) {
      ResultProgramNeed need;
      need.tiles.push_back({1, true, false, {}});
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (stage == 2) {
      double control = 0;
      check(phase.read_tensor(1, 0, {0}, &control, sizeof(control)));
      source = phase.query.tile_unit + (control >= 1 ? 2 : 0);
      ResultProgramNeed need;
      if (phase.query.input_tile_grids[0])
        need.tiles.push_back(
            {0, false, false,
             take(TileSet::from_indices(phase.resources, {source}))});
      else
        need.tiles.push_back({0, true, false, {}});
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (gate->fail.load())
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed, "fixture failure"});
    {
      std::unique_lock<std::mutex> lock(gate->mutex);
      if (gate->armed) {
        gate->armed = false;
        gate->entered = true;
        gate->changed.notify_all();
        gate->changed.wait(lock, [&] { return gate->release; });
      }
    }
    double value = 0;
    check(phase.read_tensor(0, 0, {0, source * 2, 0}, &value, sizeof(value)));
    const auto& region = phase.query.tensor_outputs->boxes()[0];
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    std::vector<double> values(take(region.element_count()), value);
    check(builder.publish_tensor(
        0, region,
        ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                 values.size() * sizeof(double))));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
struct FieldProgram {
  explicit FieldProgram(bool whole) : whole(whole) {}
  bool whole, requested = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back({0, whole, !whole, {}});
      return Result<ResultProgramPoll>(std::move(need));
    }
    const auto facts =
        whole ? multi_result::take(phase.results.at(0).descriptor())
              : phase.descriptors->at(0);
    multi_result::Program result(-1, facts.rows(0));
    return result.poll(phase);
  }
};
struct FieldProducer {
  bool requested = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using multi_result::check;
    using multi_result::take;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back({0, true, false, {}});
      return Result<ResultProgramPoll>(std::move(need));
    }
    double control = 0;
    check(phase.read_tensor(0, 0, {0}, &control, sizeof(control)));
    const auto& schema = *phase.query.output.result_schema;
    const auto rows =
        schema.fields[0].rows.kind == ResultExtentKind::RuntimeCount
            ? static_cast<std::uint64_t>(control)
            : 1;
    auto builder = take(ResultBuilder::start(phase.resources, schema,
                                             phase.query.semantic_key));
    std::vector<double> values(rows, control);
    if (rows)
      check(builder.append(
          0, rows,
          ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                   rows * sizeof(double))));
    check(builder.publish(0, rows));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
struct Fixture {
  SchemaTemplate data = multi_result::schema(ElementType::Float64, {1, 8, 1});
  SchemaTemplate output = multi_result::schema(ElementType::Float64, {1, 4, 1});
  std::shared_ptr<OperationRegistry> operations =
      std::make_shared<OperationRegistry>();
  std::unique_ptr<GraphContext> graph;
  std::unique_ptr<ExecutionContext> context;
  ExecutionPlan plan;
  ResourceBudget root;
  Gate gate;
  explicit Fixture(bool tiled_data = true, bool tiled_output = true) {
    data.tensors[0].layout.spatial = tiled_data;
    output.tensors[0].layout.spatial = tiled_output;
    OperationDefinition operation;
    operation.key = "test.tile_demand";
    operation.traits.input_count = 2;
    operation.traits.input_schema.resize(2);
    for (auto& input : operation.traits.input_schema) {
      input.kind = OperationPortKind::Result;
      input.tensor_key = "number";
    }
    operation.traits.outputs = {multi_result::output("value", output)};
    operation.traits.outputs[0].maximum_dependency_stages = 3;
    operation.traits.cacheable = false;
    operation.start_result = [&](const auto&, const auto& allocator) {
      return ResultContinuation::make<Program>(allocator, &gate);
    };
    multi_result::check(operations->register_operation(std::move(operation)));
    multi_result::check(operations->freeze());
    WorkflowDocument document;
    document.inputs = {multi_result::declaration(1, "data", data),
                       multi_result::declaration(2, "control")};
    document.nodes = {{1,
                       "test.tile_demand",
                       {WorkflowInputReference{1}, WorkflowInputReference{2}},
                       {}}};
    document.outputs = {{"result", 1, "value"}};
    graph = std::make_unique<GraphContext>(std::move(document));
    PlanningOptions planning;
    planning.tile_extent = 2;
    plan =
        multi_result::take(Compiler(operations).compile(*graph, planning)).plan;
    ExecutionContextConfig config;
    config.cpu_workers = 2;
    context = std::make_unique<ExecutionContext>(operations, config);
    root = multi_result::take(context->resource_budget());
  }
  ExecutionBindings bindings(std::vector<double> values, double control,
                             bool padded = false, unsigned tiles = 4) {
    auto schema = data;
    if (padded) {
      schema.tensors[0].layout.order = ImagePlaneOrder::Continuous;
      schema.tensors[0].layout.row_pitch_bytes = 128;
    }
    auto builder =
        multi_result::take(ResultBuilder::start(root, schema, "source"));
    if (tiles)
      multi_result::check(builder.publish_tensor(
          0, Region({{0, 1}, {0, tiles * 2}, {0, 1}}),
          ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                   tiles * 2 * sizeof(double))));
    return {{{"data", multi_result::take(builder.seal())},
             multi_result::binding(root, "control", control)}};
  }
  DemandQuery query(std::vector<TileIndex> tiles) {
    return {{"result",
             {false, multi_result::take(TileSet::from_indices(root, tiles))}}};
  }
};
bool equals(const TileSet& actual, std::vector<TileIndex> wanted) {
  return actual.size() == wanted.size() &&
         std::equal(actual.begin(), actual.end(), wanted.begin());
}
int overlapping_queries() {
  Fixture fixture;
  std::vector<double> values{10, 10, 20, 20, 30, 30, 40, 40};
  auto demand = multi_result::take(
      fixture.context->open_demand(fixture.plan, fixture.bindings(values, 0)));
  const auto all = fixture.query({0, 1}), first = fixture.query({0}),
             second = fixture.query({1});
  auto initial = demand.request(all);
  PS_REQUIRE_OK(initial);
  PS_CHECK(initial.value().generation == 1);
  auto frozen = demand.freeze();
  PS_REQUIRE_OK(frozen);
  auto equivalent = fixture.bindings(values, 0, true);
  std::reverse(equivalent.inputs.begin(), equivalent.inputs.end());
  auto equal = demand.replace_bindings(std::move(equivalent));
  PS_REQUIRE_OK(equal);
  PS_CHECK(equals(equal.value().coverage.at("result"), {0, 1}) &&
           equal.value().potential_dirty.at("result").empty());
  auto rerouted = demand.replace_bindings(fixture.bindings(values, 1));
  PS_REQUIRE_OK(rerouted);
  PS_CHECK(equals(rerouted.value().potential_dirty.at("result"), {0, 1}));
  PS_REQUIRE_OK(demand.request(first));
  PS_REQUIRE_OK(demand.request(second));
  values[0] = 999;
  auto obsolete = demand.replace_bindings(fixture.bindings(values, 1));
  PS_REQUIRE_OK(obsolete);
  PS_CHECK(obsolete.value().potential_dirty.at("result").empty());
  fixture.gate.fail = true;
  auto failed = demand.request(first);
  fixture.gate.fail = false;
  PS_CHECK(failed.status().code == ErrorCode::OperationFailed);
  values[5] = 31;  // Granted tile 2; the operation only reads sample 4.
  auto changed = demand.replace_bindings(fixture.bindings(values, 1));
  PS_REQUIRE_OK(changed);
  PS_CHECK(equals(changed.value().potential_dirty.at("result"), {0}));
  const auto generation = demand.generation().take_value();
  auto refused = demand.replace_bindings(fixture.bindings(values, 1), {0, {}});
  PS_CHECK(refused.status().code == ErrorCode::ResourceExhausted &&
           demand.generation().value() == generation);
  auto old = fixture.context->execute_fragments(frozen.value(), all);
  PS_REQUIRE_OK(old);
  PS_CHECK(multi_result::number(old.value().results.at("result"), {0, 0, 0}) ==
           10);
  PS_CHECK(demand.release(all).ok() && demand.release(first).ok() &&
           demand.release(second).ok());
  PS_CHECK(demand.release(first).code == ErrorCode::NotFound);
  auto released = demand.replace_bindings(fixture.bindings(values, 0), {0, {}});
  PS_REQUIRE_OK(released);
  PS_CHECK(released.value().coverage.empty() &&
           released.value().potential_dirty.empty());
  return 0;
}
int sparse_and_whole() {
  const std::vector<double> values{10, 10, 20, 20, 30, 30, 40, 40};
  Fixture sparse;
  auto demand = multi_result::take(sparse.context->open_demand(
      sparse.plan, sparse.bindings(values, 0, false, 1)));
  PS_REQUIRE_OK(demand.request(sparse.query({0})));
  auto expanded = demand.replace_bindings(sparse.bindings(values, 0));
  PS_REQUIRE_OK(expanded);
  PS_CHECK(expanded.value().potential_dirty.at("result").empty());
  const auto generation = demand.generation().value();
  auto missing = demand.replace_bindings(sparse.bindings(values, 0, false, 0));
  PS_CHECK(missing.status().code == ErrorCode::InvalidArgument &&
           demand.generation().value() == generation);
  Fixture whole(false, false);
  auto coarse = multi_result::take(
      whole.context->open_demand(whole.plan, whole.bindings(values, 0)));
  PS_REQUIRE_OK(coarse.request({{"result", {true, {}}}}));
  auto edited = values;
  edited[7] = 41;
  auto changed = coarse.replace_bindings(whole.bindings(edited, 0));
  PS_REQUIRE_OK(changed);
  PS_CHECK(equals(changed.value().coverage.at("result"), {0}) &&
           equals(changed.value().potential_dirty.at("result"), {0}));
  return 0;
}
int generation_race(bool cancel_run = false) {
  Fixture fixture;
  const std::vector<double> values{10, 10, 20, 20, 30, 30, 40, 40};
  auto demand = multi_result::take(
      fixture.context->open_demand(fixture.plan, fixture.bindings(values, 0)));
  const auto query = fixture.query({0});
  PS_REQUIRE_OK(demand.request(query));
  fixture.gate.armed = true;
  CancellationSource cancellation;
  auto old = std::async(std::launch::async, [&] {
    return demand.request(query, cancellation.token());
  });
  bool entered;
  {
    std::unique_lock<std::mutex> lock(fixture.gate.mutex);
    entered = fixture.gate.changed.wait_for(
        lock, std::chrono::seconds(5), [&] { return fixture.gate.entered; });
  }
  if (!entered) {
    {
      std::lock_guard<std::mutex> lock(fixture.gate.mutex);
      fixture.gate.armed = false;
      fixture.gate.release = true;
    }
    fixture.gate.changed.notify_all();
    (void)old.get();
    PS_CHECK(entered);
  }
  if (cancel_run)
    cancellation.cancel();
  const bool still_waiting =
      old.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout;
  auto replaced = demand.replace_bindings(fixture.bindings(values, 1));
  auto fresh = demand.request(query);
  {
    std::lock_guard<std::mutex> lock(fixture.gate.mutex);
    fixture.gate.release = true;
  }
  fixture.gate.changed.notify_all();
  auto stale = old.get();
  PS_CHECK(entered && still_waiting);
  PS_REQUIRE_OK(replaced);
  PS_REQUIRE_OK(fresh);
  PS_CHECK(stale.status().code ==
               (cancel_run ? ErrorCode::Cancelled : ErrorCode::Stale) &&
           fresh.value().generation == 2);
  auto edit = values;
  edit[1] = 11;
  auto update = demand.replace_bindings(fixture.bindings(edit, 1));
  PS_REQUIRE_OK(update);
  PS_CHECK(update.value().potential_dirty.at("result").empty());
  return 0;
}
int field_descriptors() {
  for (bool whole : {false, true}) {
    auto registry = std::make_shared<OperationRegistry>();
    SchemaTemplate schema;
    schema.id = "test.tile_fields";
    schema.fields = {
        {"rows", ElementType::Float64, {ResultExtentKind::RuntimeCount, 0}, {}},
        {"other", ElementType::Float64, {ResultExtentKind::Fixed, 1}, {}}};
    OperationDefinition definition;
    definition.key = "test.field_demand";
    definition.traits.input_count = 1;
    definition.traits.input_schema.resize(1);
    definition.traits.input_schema[0].kind = OperationPortKind::Result;
    definition.traits.input_schema[0].result_schema_id = schema.id;
    definition.traits.input_schema[0].result_schema_version = schema.version;
    definition.traits.outputs = {multi_result::output("value")};
    definition.traits.cacheable = false;
    definition.traits.workspace_bytes = 4096;
    definition.start_result = [whole](const auto&, const auto& allocator) {
      return ResultContinuation::make<FieldProgram>(allocator, whole);
    };
    PS_CHECK(registry->register_operation(std::move(definition)).ok());
    PS_CHECK(registry->freeze().ok());
    WorkflowDocument document;
    document.inputs = {multi_result::declaration(1, "rows", schema)};
    document.nodes = {
        {1, "test.field_demand", {WorkflowInputReference{1}}, {}}};
    document.outputs = {{"result", 1, "value"}};
    GraphContext graph(std::move(document));
    auto compiled = Compiler(registry).compile(graph);
    PS_REQUIRE_OK(compiled);
    ExecutionContext context(registry);
    auto root = context.resource_budget().take_value();
    const auto bindings = [&](std::uint64_t rows, double other) {
      auto builder =
          multi_result::take(ResultBuilder::start(root, schema, "fields"));
      for (std::uint32_t slot = 0; slot < 2; ++slot) {
        const auto count = slot ? 1 : rows;
        std::vector<double> values(count, slot ? other : 7);
        if (count)
          multi_result::check(builder.append(
              slot, count,
              ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                       count * sizeof(double))));
        multi_result::check(builder.publish(slot, count));
      }
      return ExecutionBindings{{{"rows", multi_result::take(builder.seal())}}};
    };
    auto demand = multi_result::take(
        context.open_demand(compiled.value().plan, bindings(2, 10)));
    const DemandQuery query{{"result", {true, {}}}};
    PS_REQUIRE_OK(demand.request(query));
    auto bytes = demand.replace_bindings(bindings(2, 11));
    PS_REQUIRE_OK(bytes);
    PS_CHECK(
        equals(bytes.value().potential_dirty.at("result"),
               whole ? std::vector<TileIndex>{0} : std::vector<TileIndex>{}));
    PS_REQUIRE_OK(demand.request(query));
    auto facts = demand.replace_bindings(bindings(3, 11));
    PS_REQUIRE_OK(facts);
    PS_CHECK(equals(facts.value().potential_dirty.at("result"), {0}));
    const auto generation = demand.generation().value();
    auto wrong =
        demand.replace_bindings({{multi_result::binding(root, "rows", 1)}});
    PS_CHECK(wrong.status().code == ErrorCode::TypeMismatch &&
             demand.generation().value() == generation);
  }
  return 0;
}
int computed_descriptors() {
  for (bool dynamic : {false, true}) {
    auto registry = std::make_shared<OperationRegistry>();
    SchemaTemplate rows;
    rows.id = "test.computed_rows";
    rows.fields.push_back(
        {"rows",
         ElementType::Float64,
         {dynamic ? ResultExtentKind::RuntimeCount : ResultExtentKind::Fixed,
          1},
         {}});
    OperationDefinition producer;
    producer.key = "test.field_producer";
    producer.traits.input_count = 1;
    producer.traits.input_schema.resize(1);
    producer.traits.input_schema[0].kind = OperationPortKind::Result;
    producer.traits.input_schema[0].tensor_key = "number";
    producer.traits.outputs = {multi_result::output("rows", rows)};
    producer.traits.cacheable = true;
    producer.start_result = [](const auto&, const auto& allocator) {
      return ResultContinuation::make<FieldProducer>(allocator);
    };
    PS_CHECK(registry->register_operation(std::move(producer)).ok());
    OperationDefinition consumer;
    consumer.key = "test.field_consumer";
    consumer.traits.input_count = 1;
    consumer.traits.input_schema.resize(1);
    consumer.traits.input_schema[0].kind = OperationPortKind::Result;
    consumer.traits.input_schema[0].result_schema_id = rows.id;
    consumer.traits.input_schema[0].result_schema_version = rows.version;
    consumer.traits.outputs = {multi_result::output("value")};
    consumer.traits.cacheable = true;
    consumer.traits.workspace_bytes = 4096;
    consumer.start_result = [](const auto&, const auto& allocator) {
      return ResultContinuation::make<FieldProgram>(allocator, false);
    };
    PS_CHECK(registry->register_operation(std::move(consumer)).ok());
    PS_CHECK(registry->freeze().ok());
    WorkflowDocument document;
    document.inputs = {multi_result::declaration(1, "control")};
    document.nodes = {
        {1, "test.field_producer", {WorkflowInputReference{1}}, {}},
        {2, "test.field_consumer", {WorkflowNodeOutput{1, "rows"}}, {}}};
    document.outputs = {{"result", 2, "value"}, {"rows", 1, "rows"}};
    GraphContext graph(std::move(document));
    auto compiled = Compiler(registry).compile(graph);
    PS_REQUIRE_OK(compiled);
    ExecutionContextConfig config;
    config.result_cache_bytes = 16 * 1024 * 1024;
    ExecutionContext context(registry, config);
    auto root = context.resource_budget().take_value();
    auto demand = multi_result::take(context.open_demand(
        compiled.value().plan, {{multi_result::binding(root, "control", 0)}}));
    const DemandQuery query{{"result", {true, {}}}, {"rows", {true, {}}}};
    auto initial = demand.request(query);
    PS_REQUIRE_OK(initial);
    PS_CHECK(multi_result::number(initial.value().results.at("result")) ==
             (dynamic ? 0 : 1));
    auto cached = demand.request(query);
    PS_REQUIRE_OK(cached);
    PS_CHECK(cached.value().diagnostics.cache_hits >= 1);
    auto changed =
        demand.replace_bindings({{multi_result::binding(root, "control", 1)}});
    PS_REQUIRE_OK(changed);
    PS_CHECK(
        equals(changed.value().potential_dirty.at("result"),
               dynamic ? std::vector<TileIndex>{0} : std::vector<TileIndex>{}));
    PS_CHECK(equals(changed.value().potential_dirty.at("rows"), {0}));
    auto current = demand.request(query);
    PS_REQUIRE_OK(current);
    PS_CHECK(multi_result::number(current.value().results.at("result")) == 1);
  }
  return 0;
}
int limits_bits_and_retirement() {
  Fixture fixture;
  std::vector<double> values{10, 0, 20, 20, 30, 30, 40, 40};
  const auto query = fixture.query({0});
  auto bounded = multi_result::take(fixture.context->open_demand(
      fixture.plan, fixture.bindings(values, 0), {1}));
  auto refused = bounded.request(query);
  PS_CHECK(refused.status().code == ErrorCode::ResourceExhausted &&
           bounded.generation().value() == 1);
  auto no_subscription =
      bounded.replace_bindings(fixture.bindings(values, 1), {0, {}});
  PS_REQUIRE_OK(no_subscription);
  PS_CHECK(no_subscription.value().coverage.empty());
  auto demand = multi_result::take(
      fixture.context->open_demand(fixture.plan, fixture.bindings(values, 0)));
  PS_REQUIRE_OK(demand.request(query));
  for (std::uint64_t bits :
       {UINT64_C(0x8000000000000000), UINT64_C(0x7ff8000000000001),
        UINT64_C(0x7ff8000000000002)}) {
    std::memcpy(&values[1], &bits, sizeof(bits));
    auto changed = demand.replace_bindings(fixture.bindings(values, 0));
    PS_REQUIRE_OK(changed);
    PS_CHECK(equals(changed.value().potential_dirty.at("result"), {0}));
    PS_REQUIRE_OK(demand.request(query));
  }
  auto packed = fixture.bindings(values, 0);
  auto builder = multi_result::take(
      ResultBuilder::start(fixture.root, fixture.data, "reverse"));
  auto storage =
      multi_result::take(fixture.root.allocator().allocate(8 * sizeof(double)));
  for (std::size_t i = 0; i < values.size(); ++i)
    std::memcpy(storage.data() + i * sizeof(double), &values[7 - i],
                sizeof(double));
  multi_result::check(builder.publish_tensor(0, Region::whole({1, 8, 1}),
                                             {56, {64, -8, 8}},
                                             std::move(storage).freeze()));
  packed.inputs[0].result = multi_result::take(builder.seal());
  auto reversed = demand.replace_bindings(packed);
  PS_REQUIRE_OK(reversed);
  PS_CHECK(reversed.value().potential_dirty.at("result").empty());
  CancellationSource cancelled;
  cancelled.cancel();
  const auto generation = demand.generation().value();
  auto stopped = demand.replace_bindings(fixture.bindings(values, 1),
                                         {0, cancelled.token()});
  PS_CHECK(stopped.status().code == ErrorCode::Cancelled &&
           demand.generation().value() == generation);
  unsigned retired = 0;
  const auto ordinary = fixture.bindings(values, 0);
  auto observer =
      multi_result::take(fixture.context->open_demand(fixture.plan, ordinary));
  const auto owned = [&] {
    BufferAllocator allocator([&](std::uint64_t) {
      return Result<std::shared_ptr<void>>(
          std::shared_ptr<void>(new int(0), [&, observer](void* pointer) {
            delete static_cast<int*>(pointer);
            auto state = observer.generation();
            if (state.ok() || state.status().code == ErrorCode::Cancelled)
              ++retired;
          }));
    });
    auto mutable_value = multi_result::take(
        MutableValue::allocate(fixture.data.tensors[0].descriptor,
                               Region::whole({1, 8, 1}), allocator));
    std::memset(mutable_value.data(), 0, mutable_value.size());
    auto value = multi_result::take(std::move(mutable_value).publish());
    auto result = multi_result::take(
        ResultBuilder::start(fixture.root, fixture.data, "owned"));
    multi_result::check(result.publish_tensor(
        0, value.region(), value.layout(),
        multi_result::take(fixture.root.reference(value.storage()))));
    auto inputs = ordinary;
    inputs.inputs[0].result = multi_result::take(result.seal());
    return multi_result::take(
        fixture.context->open_demand(fixture.plan, std::move(inputs)));
  };
  auto cancelled_owner = owned();
  PS_CHECK(cancelled_owner.cancel() && retired == 1);
  auto replaced_owner = owned();
  PS_REQUIRE_OK(replaced_owner.replace_bindings(ordinary));
  PS_CHECK(retired == 2);
  auto destroyed = owned();
  destroyed = {};
  PS_CHECK(retired == 3);
  auto closing = owned();
  fixture.context.reset();
  PS_CHECK(retired == 4 &&
           closing.generation().status().code == ErrorCode::Cancelled);
  return 0;
}
}  // namespace
int main() {
  return overlapping_queries() || sparse_and_whole() || generation_race() ||
         generation_race(true) || field_descriptors() ||
         computed_descriptors() || limits_bits_and_retirement();
}
