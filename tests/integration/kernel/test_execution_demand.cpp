#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "photospider/plugin/whole_stage.hpp"
#include "support/conditional_sum_fixture.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/result_input_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
TileSet point(std::uint64_t at, std::uint64_t size = 5) {
  if (at >= size)
    throw std::runtime_error("test tile outside domain");
  return multi_result::take(TileSet::from_indices(ResourceBudget{}, {at}));
}
TileSet all_tiles(std::uint64_t size) {
  std::vector<TileIndex> indices;
  for (TileIndex i = 0; i < size; ++i)
    indices.push_back(i);
  return multi_result::take(
      TileSet::from_indices(ResourceBudget{}, indices.data(), indices.size()));
}
TileSet dirty_tiles(const ExecutionDependencies& dependencies,
                    std::size_t input, const TileSet& changed,
                    std::size_t step = 0) {
  ResourceBudget root;
  ResourceVector<TileChange> changes{ResourceAllocator<TileChange>(root)};
  changes.push_back({PlanWorkflowInput{input}, 0, 0, changed, false});
  auto dirty = multi_result::take(dependencies.dirty_units(changes));
  std::vector<TileIndex> tiles;
  for (const auto& unit : dirty)
    if (unit.step == step)
      tiles.push_back(unit.tile);
  return multi_result::take(
      TileSet::from_indices(root, tiles.data(), tiles.size()));
}
template <class T>
TensorSpan values(ElementType type, const std::vector<T>& numbers) {
  auto writer = TensorWriter::allocate({type, {1, numbers.size()}},
                                       Region::whole({1, numbers.size()}),
                                       BufferAllocator{})
                    .take_value();
  std::memcpy(writer.data(), numbers.data(), writer.size());
  return std::move(writer).publish().take_value();
}
ResultRef source(const ResourceBudget& root, const TensorSpan& backing) {
  // Immutable caller backing stays in Root Referenced accounting.
  auto schema = result_input_fixture::source_schema(backing);
  schema.tensors[0].layout.spatial = true;
  schema.tensors[0].layout.channel_axis.reset();
  auto builder = multi_result::take(
      ResultBuilder::start(root, schema, "test.source", {}, 1));

  multi_result::check(builder.publish_tensor(
      0, backing.region(), backing.layout(),
      multi_result::take(root.reference(backing.storage()))));
  return multi_result::take(builder.seal());
}
WorkflowDocument routing_document() {
  WorkflowDocument document;
  result_input_fixture::declare_sources(
      &document, {values<double>(ElementType::Float64, {1, 2, 3, 0, 5}),
                  values<std::int64_t>(ElementType::Int64, {0, 0, 0, 0, 0})});
  for (auto& input : document.inputs) {
    auto spatial = std::make_shared<SchemaTemplate>(*input.result_schema);
    spatial->tensors[0].layout.spatial = true;
    spatial->tensors[0].layout.channel_axis.reset();
    input.result_schema = std::move(spatial);
  }
  document.inputs[0].name = "data";
  document.inputs[1].name = "control";
  document.nodes = {{1,
                     "test.conditional_sum",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"sum", 1, "value"}};
  return document;
}
int generations() {
  auto registry = ps::test::conditional_sum_registry();
  GraphContext graph(routing_document());
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan;
  ExecutionContext context(registry, {1, false, 8, 4096});
  const auto root = context.resource_budget().take_value();
  ExecutionBindings bindings{
      {{"data",
        source(root, values<double>(ElementType::Float64, {1, 2, 3, 0, 5}))},
       {"control", source(root, values<std::int64_t>(ElementType::Int64,
                                                     {0, 0, 0, 0, 0}))}}};
  auto opened = context.open_demand(plan, bindings);
  PS_CHECK(opened.ok());
  auto demand = opened.take_value();
  PS_CHECK(demand.generation().value() == 1);
  const auto q = point(0).unite(point(4), ResourceBudget{}).take_value();
  DemandQuery query{{"sum", TileRequest{{{0, q}}}}};
  auto before = demand.request(query);
  PS_REQUIRE_OK(before);
  PS_CHECK(before.ok() && before.value().generation == 1);
  PS_CHECK(before.value()
               .results.at("sum")
               .descriptor()
               .take_value()
               .tensor_coverage(0) ==
           multi_result::take(Footprint::from_regions(
               {1, 5}, {Region({{0, 1}, {0, 1}}), Region({{0, 1}, {4, 1}})})));
  double result = 0;
  PS_CHECK(result_input_fixture::read(before.value().results.at("sum"), {0, 0},
                                      &result, 8)
               .ok() &&
           result == 1);
  PS_CHECK(!result_input_fixture::read(before.value().results.at("sum"), {0, 1},
                                       &result, 8)
                .ok());
  auto retained = demand.request(query);
  PS_CHECK(retained.ok() &&
           !retained.value().diagnostics.operation_timings.empty() &&
           retained.value().diagnostics.cache_hits == 0 &&
           result_input_fixture::read(retained.value().results.at("sum"),
                                      {0, 0}, &result, 8)
               .ok() &&
           result == 1);
  auto frozen = demand.freeze().take_value();
  bindings.inputs[1].result =
      source(root, values<std::int64_t>(ElementType::Int64, {0, 0, 0, 1, 0}));
  auto edit = demand.replace_bindings(bindings);
  if (!edit.ok())
    std::cerr << edit.status().message << '\n';
  PS_CHECK(edit.ok() && edit.value().generation == 2 &&
           edit.value().coverage.at("sum").members.at(0) == q);
  PS_CHECK(edit.value().potential_dirty.at("sum").members.at(0) == q);
  auto same_value = demand.request(query);
  PS_CHECK(same_value.ok() &&
           result_input_fixture::read(same_value.value().results.at("sum"),
                                      {0, 0}, &result, 8)
               .ok() &&
           result == 1);
  PS_CHECK(dirty_tiles(same_value.value().dependencies, 0, point(3), 0) == q);
  auto old = context.execute_fragments(frozen, query);
  PS_CHECK(old.ok() && old.value().generation == 0);
  PS_CHECK(dirty_tiles(old.value().dependencies, 0, point(3), 0) == q);
  bindings.inputs[0].result =
      source(root, values<double>(ElementType::Float64, {1, 2, 3, 9, 5}));
  auto changed = demand.replace_bindings(bindings);
  PS_CHECK(changed.ok() &&
           changed.value().potential_dirty.at("sum").members.at(0) == q);
  auto fresh = demand.request(query);
  PS_CHECK(fresh.ok() &&
           result_input_fixture::read(fresh.value().results.at("sum"), {0, 4},
                                      &result, 8)
               .ok() &&
           result == 14);
  // Static grants compare control[0..4] and data{0,3,4} for either control
  // value.
  bindings.inputs[0].result =
      source(root, values<double>(ElementType::Float64, {1, 2, 777, 9, 5}));
  auto unrelated =
      demand.replace_bindings(bindings, SnapshotAccessOptions{8, {}});
  PS_CHECK(unrelated.ok() &&
           unrelated.value().potential_dirty.at("sum").empty());
  auto comparison = bindings;
  comparison.inputs[0].result =
      source(root, values<double>(ElementType::Float64, {1, 2, 777, 9, 5}));
  comparison.inputs[1].result =
      source(root, values<std::int64_t>(ElementType::Int64, {0, 0, 0, 1, 0}));
  auto failed =
      demand.replace_bindings(comparison, SnapshotAccessOptions{7, {}});
  PS_CHECK(failed.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(demand.generation().value() == unrelated.value().generation);
  auto bad_bindings = bindings;
  bad_bindings.inputs[0].result =
      source(root, values<double>(ElementType::Float64, {1}));
  PS_CHECK(demand.replace_bindings(bad_bindings).status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(demand.release(query).ok());
  PS_CHECK(demand.release(query).code == ErrorCode::NotFound);
  auto no_subscriptions = demand.replace_bindings(bindings);
  PS_CHECK(no_subscriptions.ok() && no_subscriptions.value().coverage.empty());
  auto empty = demand.request({{"sum", TileRequest{}}});
  PS_CHECK(empty.ok() && empty.value()
                             .results.at("sum")
                             .descriptor()
                             .take_value()
                             .tensor_coverage(0)
                             .empty());
  PS_CHECK(
      !multi_result::take(empty.value().dependencies.tile_record({0, 0, 0})));
  PS_CHECK(demand.request({}).ok());
  PS_CHECK(demand.cancel() && !demand.cancel());
  PS_CHECK(demand.request(query).status().code == ErrorCode::Cancelled);
  PS_CHECK(context.execute_fragments(frozen, query).ok());
  return 0;
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned entered = 0;
  bool release = false;
  bool ignore_cancellation = false;
  unsigned released = 0;
  std::atomic<unsigned> active{0};
  std::atomic<unsigned> narrow_starts{0}, wide_starts{0};
  bool await(unsigned count) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(3),
                            [&] { return entered >= count; });
  }
  void open() {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
    changed.notify_all();
  }
  void allow(unsigned count) {
    std::lock_guard<std::mutex> lock(mutex);
    released = count;
    changed.notify_all();
  }
};
SchemaTemplate tensor_schema(ElementType type = ElementType::Float64,
                             std::uint64_t count = 2) {
  auto schema = multi_result::schema(type, {1, count});
  schema.tensors[0].layout.spatial = true;
  schema.tensors[0].layout.channel_axis.reset();
  schema.id = "test.result.input";
  schema.tensors[0].key = "data";
  return schema;
}
ExecutionBinding binding(const ExecutionContext& context, std::string name,
                         const TensorSpan& backing) {
  return {std::move(name),
          source(multi_result::take(context.resource_budget()), backing)};
}
ExecutionBinding snapshot_binding(const ExecutionContext& context,
                                  std::string name,
                                  const InputSnapshot& snapshot,
                                  const TensorSpan& prototype) {
  std::vector<std::uint8_t> bytes(prototype.bytes().size());
  multi_result::check(
      snapshot.read(prototype.region(), bytes.data(), bytes.size()));
  auto backing = multi_result::take(
      TensorSpan::create(prototype.descriptor(), prototype.region(),
                         prototype.layout(), std::move(bytes)));
  return binding(context, std::move(name), backing);
}
WorkflowDocument identity_document(const TensorSpan& value) {
  WorkflowDocument document;
  result_input_fixture::declare_sources(&document, {value});
  for (auto& input : document.inputs) {
    auto spatial = std::make_shared<SchemaTemplate>(*input.result_schema);
    spatial->tensors[0].layout.spatial = true;
    spatial->tensors[0].layout.channel_axis.reset();
    input.result_schema = std::move(spatial);
  }
  document.inputs[0].name = "image";
  document.nodes = {
      {1, "test.snapshot_identity", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"result", 1, "value"}};
  return document;
}
struct GatedProgram {
  std::shared_ptr<Gate> gate;
  std::shared_ptr<std::atomic<unsigned>> effects;
  bool whole = false, staged = false;
  unsigned stage = 0;
  GatedProgram(std::shared_ptr<Gate> gate, bool whole, bool staged,
               std::shared_ptr<std::atomic<unsigned>> effects = {})
      : gate(std::move(gate)),
        effects(std::move(effects)),
        whole(whole),
        staged(staged) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    const auto& schema = *phase.query.output.result_schema;
    const auto shape = schema.tensors[0].sample_shape();
    const auto outputs = phase.query.tensor_outputs.value_or(
        multi_result::take(Footprint::all(shape)));
    const auto wanted =
        whole ? multi_result::take(Footprint::all(shape)) : outputs;
    if (!effects && !outputs.empty() && stage < (staged ? 2U : 1U)) {
      const bool descriptor_only = staged && stage == 0;
      ++stage;
      return Answer(
          ResultProgramNeed{{multi_result::take(sdk::tile_need_for_samples(
              phase.resources, phase.query, 0, 0, wanted, descriptor_only))}});
    }
    if (gate && !outputs.empty()) {
      ++gate->active;
      struct Active {
        Gate* gate;
        ~Active() { --gate->active; }
      } active{gate.get()};
      std::unique_lock<std::mutex> lock(gate->mutex);
      const auto ticket = ++gate->entered;
      gate->changed.notify_all();
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!gate->release && ticket > gate->released &&
             (!phase.query.cancellation.cancelled() ||
              gate->ignore_cancellation)) {
        if (std::chrono::steady_clock::now() >= deadline)
          break;
        gate->changed.wait_for(lock, std::chrono::milliseconds(2));
      }
      if (phase.query.cancellation.cancelled())
        return Answer(Status{ErrorCode::Cancelled, {}});
      if (!gate->release && ticket > gate->released)
        return Answer(Status{ErrorCode::OperationFailed, "gate deadline"});
    }
    auto builder = multi_result::take(
        ResultBuilder::start(phase.resources, schema, phase.query.semantic_key,
                             {}, phase.query.tile_extent));

    const double effect = effects ? ++*effects : 0;
    for (const auto& box : outputs.boxes()) {
      auto bytes = multi_result::take(phase.allocator.allocate(
          multi_result::take(box.element_count()) * 8));
      for (std::uint64_t i = 0; i < box.dimensions()[1].extent; ++i) {
        const auto at = box.dimensions()[1].offset + i;
        double value = effect;
        if (!effects) {
          auto status = phase.read_tensor(0, 0, {0, at}, &value, 8);
          if (!status.ok())
            return Answer(status);
          if (!std::isfinite(value))
            return Answer(Status{ErrorCode::OperationFailed,
                                 "nonfinite sample " + std::to_string(at)});
        }
        std::memcpy(bytes.data() + i * 8, &value, 8);
      }
      multi_result::check(builder.publish_tensor(
          0, box, {0, {8, 8}, {0, box.dimensions()[1].offset}},
          std::move(bytes).freeze()));
    }
    return Answer(ResultPublication{multi_result::take(builder.seal()), true});
  }
};
OperationDefinition copy_definition(const std::string& key,
                                    std::uint64_t state) {
  OperationDefinition op;
  op.key = key;
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);

  op.traits.input_schema[0].rank = 2;
  op.traits.outputs[0] = multi_result::output("value", tensor_schema());
  op.traits.outputs[0].continuation_bytes = state;
  op.traits.outputs[0].maximum_dependency_stages = 4;
  op.map_dependencies = sdk::pointwise_dependencies;
  op.traits.workspace_bytes = 256;
  op.traits.requires_metadata_specialization = true;
  op.traits.outputs[0].output_schema.result_schema_id.clear();
  op.traits.outputs[0].output_schema.result_schema_version = 0;
  op.traits.outputs[0].output_schema.rank = 2;
  op.specialize_metadata = [](const std::vector<OperationMetadata>& inputs,
                              const std::map<std::string, ParameterValue>&) {
    OperationOutputSpecialization output;
    output.metadata = inputs[0];
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  return op;
}
std::shared_ptr<OperationRegistry> gated_registry(
    const std::shared_ptr<Gate>& gate, bool staged = false,
    std::shared_ptr<std::atomic<unsigned>> effects = {}, bool whole = false,
    bool gpu_fallback = false) {
  auto registry = std::make_shared<OperationRegistry>();
  auto op = copy_definition("wait", sizeof(GatedProgram));
  op.traits.supports_gpu = gpu_fallback;
  op.traits.allows_cpu_fallback = gpu_fallback;
  if (whole) {
    op.specialize_metadata = [](const std::vector<OperationMetadata>& inputs,
                                const std::map<std::string, ParameterValue>&) {
      auto schema = std::make_shared<SchemaTemplate>(*inputs[0].result_schema);
      schema->tensors[0].layout.spatial = false;
      schema->tensors[0].atomic_trailing_axes = 2;
      OperationOutputSpecialization output;
      output.metadata.result_schema = std::move(schema);
      return Result<std::vector<OperationOutputSpecialization>>(
          std::vector<OperationOutputSpecialization>{std::move(output)});
    };
  }
  op.start_result = [gate, whole, staged](const ResultProgramQuery& query,
                                          const BufferAllocator& allocator) {
    if (gate && query.tensor_outputs) {
      const auto count =
          multi_result::take(query.tensor_outputs->element_count());
      if (count == 1)
        ++gate->narrow_starts;
      if (count == 2)
        ++gate->wide_starts;
    }
    return ResultContinuation::make<GatedProgram>(allocator, gate, whole,
                                                  staged);
  };
  if (effects) {
    OperationDefinition effect;
    effect.key = "effect";
    effect.traits.deterministic = false;
    effect.traits.side_effect_free = false;
    effect.traits.cacheable = false;
    effect.traits.workspace_bytes = 32;
    effect.traits.outputs[0] = multi_result::output("value", tensor_schema());
    effect.traits.outputs[0].continuation_bytes = sizeof(GatedProgram);
    effect.start_result = [effects](const ResultProgramQuery&,
                                    const BufferAllocator& allocator) {
      return ResultContinuation::make<GatedProgram>(allocator, nullptr, false,
                                                    false, effects);
    };
    multi_result::check(registry->register_operation(
        sdk::with_whole_stage(std::move(effect), 1000000)));
  }
  multi_result::check(registry->register_operation(std::move(op)));
  multi_result::check(registry->freeze());
  return registry;
}
WorkflowDocument gate_document() {
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "x", tensor_schema())};
  document.nodes = {{1, "wait", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"y", 1, "value"}};
  return document;
}
int edit_and_cancel() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate);
  GraphContext graph(gate_document());
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan;
  auto context = std::make_unique<ExecutionContext>(
      registry, ExecutionContextConfig{2, false, 8, 4096});
  ExecutionBindings bindings{
      {binding(*context, "x", values<double>(ElementType::Float64, {1, 2}))}};
  auto demand = multi_result::take(context->open_demand(plan, bindings));
  gate->open();
  PS_CHECK(demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}}).ok());
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->entered = 0;
    gate->release = false;
  }
  auto old = std::async(std::launch::async, [&] {
    return demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}});
  });
  PS_CHECK(gate->await(1));
  bindings.inputs[0].result =
      binding(*context, bindings.inputs[0].name,
              values<double>(ElementType::Float64, {3, 4}))
          .result;
  auto replaced = demand.replace_bindings(bindings);
  PS_CHECK(replaced.ok() && replaced.value().generation == 2);
  PS_CHECK(replaced.value().potential_dirty.at("y").members.at(0) ==
           point(0, 2));
  gate->open();
  PS_CHECK(old.get().status().code == ErrorCode::Stale && gate->active == 0);
  auto next = demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}});
  PS_CHECK(next.ok() && next.value().generation == 2);
  double value = 0;
  PS_CHECK(result_input_fixture::read(next.value().results.at("y"), {0, 0},
                                      &value, 8)
               .ok() &&
           value == 3);
  next = Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
  // Each request owns its token; cancelling one does not stop the other.
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->entered = 0;
    gate->release = false;
  }
  CancellationSource cancel;
  auto a = std::async(std::launch::async, [&] {
    return demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}});
  });
  auto b = std::async(std::launch::async, [&] {
    return demand.request({{"y", TileRequest{{{0, point(1, 2)}}}}},
                          cancel.token());
  });
  PS_CHECK(gate->await(2));
  cancel.cancel();
  gate->changed.notify_all();
  PS_CHECK(b.get().status().code == ErrorCode::Cancelled);
  gate->open();
  PS_CHECK(a.get().ok() && gate->active == 0);
  // Context shutdown propagates cancellation and waits for callback retirement.
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->entered = 0;
    gate->release = false;
  }
  auto pending = std::async(std::launch::async, [&] {
    return demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}});
  });
  PS_CHECK(gate->await(1));
  context.reset();
  PS_CHECK(pending.get().status().code == ErrorCode::Cancelled &&
           gate->active == 0);
  PS_CHECK(
      demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}}).status().code ==
      ErrorCode::Cancelled);
  return 0;
}
int replacement_during_shutdown() {
  auto registry = gated_registry(nullptr);
  GraphContext graph(gate_document());
  auto compiled = Compiler(registry).compile(
      graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1});
  PS_REQUIRE_OK(compiled);
  auto plan = compiled.take_value().plan;
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    auto context = std::make_unique<ExecutionContext>(
        registry, ExecutionContextConfig{1, false, 4, 4096});
    ExecutionBindings bindings{
        {binding(*context, "x", values<double>(ElementType::Float64, {1, 2}))}};
    auto opened = context->open_demand(plan, bindings);
    PS_REQUIRE_OK(opened);
    auto demand = opened.take_value();
    std::atomic<bool> ready{false}, begin{false};
    auto replaced = std::async(std::launch::async, [&] {
      ready = true;
      while (!begin.load())
        std::this_thread::yield();
      return demand.replace_bindings(bindings);
    });
    while (!ready.load())
      std::this_thread::yield();
    begin = true;
    context.reset();
    auto result = replaced.get();
    PS_CHECK(result.ok() || result.status().code == ErrorCode::Cancelled);
    PS_CHECK(demand.replace_bindings(bindings).status().code ==
             ErrorCode::Cancelled);
  }
  return 0;
}
int isolation_and_limits() {
  auto gate = std::make_shared<Gate>();
  gate->open();
  auto registry = gated_registry(gate);
  GraphContext graph(gate_document());
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan;
  ExecutionContextConfig config{1, false, 4, 4096};
  config.maximum_demands = 1;
  ExecutionContext context(registry, config);
  ExecutionBindings bindings{
      {binding(context, "x",
               values<double>(ElementType::Float64,
                              {1, std::numeric_limits<double>::infinity()}))}};
  auto demand = context.open_demand(plan, bindings).take_value();
  PS_CHECK(context.open_demand(plan, bindings).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}}).ok());
  PS_CHECK(
      demand.request({{"y", TileRequest{{{0, point(1, 2)}}}}}).status().code ==
      ErrorCode::OperationFailed);
  PS_CHECK(
      demand.request({{"y", TileRequest{{{0, all_tiles(2)}}}}}).status().code ==
      ErrorCode::OperationFailed);
  PS_CHECK(demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}}).ok());
  auto frozen = demand.freeze().take_value();
  PS_CHECK(graph.replace(gate_document()) > 0);
  PS_CHECK(demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}}).ok());
  PS_CHECK(demand.cancel());
  auto fresh = context.open_demand(frozen.plan(), bindings, DemandConfig{1});
  PS_CHECK(fresh.ok());
  PS_CHECK(fresh.value()
               .request({{"y", TileRequest{{{0, point(0, 2)}}}}})
               .status()
               .code == ErrorCode::ResourceExhausted);
  return 0;
}
int combined_tokens() {
  CancellationSource a, b;
  auto both = CancellationToken::combine({a.token(), b.token()}).take_value();
  auto nested = CancellationToken::combine({both, a.token(), {}}).take_value();
  PS_CHECK(!nested.cancelled());
  b.cancel();
  PS_CHECK(both.cancelled() && nested.cancelled() && !a.token().cancelled());
  std::vector<CancellationSource> sources(65);
  std::vector<CancellationToken> tokens;
  for (auto& source : sources)
    tokens.push_back(source.token());
  PS_CHECK(CancellationToken::combine(tokens).status().code ==
           ErrorCode::ResourceExhausted);
  tokens.pop_back();
  auto all = CancellationToken::combine(tokens).take_value();
  PS_CHECK(!all.cancelled());
  sources[32].cancel();
  PS_CHECK(all.cancelled());
  return 0;
}
std::shared_ptr<OperationRegistry> snapshot_registry() {
  auto registry = std::make_shared<OperationRegistry>();
  auto op =
      copy_definition("test.snapshot_identity", sizeof(multi_result::Program));
  op.traits.workspace_bytes = 4096;
  op.start_result = [](const ResultProgramQuery&,
                       const BufferAllocator& allocator) {
    return ResultContinuation::make<multi_result::Program>(allocator, 0);
  };
  multi_result::check(registry->register_operation(std::move(op)));
  multi_result::check(registry->freeze());
  return registry;
}
int snapshot_replacement() {
  auto registry = snapshot_registry();
  ExecutionContext context(registry, {1, false, 8, 16384});
  for (auto type : {ElementType::UInt8, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64}) {
    auto writer = TensorWriter::allocate({type, {1, 3}}, Region::whole({1, 3}),
                                         BufferAllocator{})
                      .take_value();
    std::memset(writer.data(), 0, writer.size());
    auto initial = std::move(writer).publish().take_value();
    InputSnapshotStore store({256, 1});
    auto snapshot =
        store
            .import_tensor(Tensor::create(initial.descriptor(),
                                          Footprint::from_regions(
                                              initial.descriptor().shape,
                                              {initial.region()})
                                              .take_value(),
                                          {initial})
                               .take_value())
            .take_value();
    auto document = identity_document(initial);
    document.nodes[0].operation = "test.snapshot_identity";
    GraphContext graph(document);
    auto plan =
        Compiler(registry)
            .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
            .take_value()
            .plan;
    ExecutionBindings bindings{
        {snapshot_binding(context, "image", snapshot, initial)}};
    auto demand = context.open_demand(plan, bindings).take_value();
    const auto q =
        point(0, 3).unite(point(2, 3), ResourceBudget{}).take_value();
    PS_CHECK(demand.request({{"result", TileRequest{{{0, q}}}}}).ok());
    auto frozen = demand.freeze().take_value();
    for (auto at : {0U, 2U}) {
      auto patch =
          TensorWriter::allocate(initial.descriptor(),
                                 Region({{0, 1}, {at, 1}}), BufferAllocator{})
              .take_value();
      // Includes nonfinite generic float payloads; dirty compares bits.
      std::memset(patch.data(), 0xff, patch.size());
      auto published_patch = std::move(patch).publish().take_value();
      snapshot =
          store
              .patch(snapshot,
                     Tensor::create(published_patch.descriptor(),
                                    Footprint::from_regions(
                                        published_patch.descriptor().shape,
                                        {published_patch.region()})
                                        .take_value(),
                                    {published_patch})
                         .take_value())
              .take_value();
      bindings.inputs[0] =
          snapshot_binding(context, "image", snapshot, initial);
      auto changed = demand.replace_bindings(bindings, {2, {}});
      PS_CHECK(changed.ok());
      PS_CHECK(changed.value().potential_dirty.at("result").members.at(0) ==
               (at == 0 ? point(0, 3) : q));
    }
    auto final = demand.request({{"result", TileRequest{{{0, q}}}}});
    PS_CHECK(final.ok());
    std::uint8_t bytes[8]{};
    const auto width = TensorSpan::element_size(type);
    PS_CHECK(result_input_fixture::read(final.value().results.at("result"),
                                        {0, 2}, bytes, width)
                 .ok());
    for (unsigned i = 0; i < width; ++i)
      PS_CHECK(bytes[i] == 0xff);
    auto old =
        context.execute_fragments(frozen, {{"result", TileRequest{{{0, q}}}}});
    PS_CHECK(old.ok() &&
             result_input_fixture::read(old.value().results.at("result"),
                                        {0, 2}, bytes, width)
                 .ok());
    for (unsigned i = 0; i < width; ++i)
      PS_CHECK(bytes[i] == 0);
    auto same = demand.replace_bindings(bindings, {2, {}});
    PS_CHECK(same.ok() && same.value().potential_dirty.at("result").empty());
  }
  return 0;
}
int concurrent_ancestors() {
  for (unsigned scenario = 0; scenario < 6; ++scenario) {
    const auto cancelled = scenario % 3;
    auto gate = std::make_shared<Gate>();
    auto registry = gated_registry(gate, scenario >= 3);
    auto document = gate_document();
    document.nodes.push_back({2, "wait", {WorkflowNodeOutput{1, "value"}}, {}});
    document.outputs = {{"inner", 1, "value"}, {"outer", 2, "value"}};
    GraphContext graph(document);
    auto plan =
        Compiler(registry)
            .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
            .take_value()
            .plan;
    ExecutionContext context(registry, {2, false, 8, 4096});
    auto demand =
        context
            .open_demand(
                plan, {{binding(context, "x",
                                values<double>(ElementType::Float64, {7, 9}))}})
            .take_value();
    CancellationSource stop_outer, stop_inner;
    auto outer = std::async(std::launch::async, [&] {
      return demand.request({{"outer", TileRequest{{{0, point(0, 2)}}}}},
                            stop_outer.token());
    });
    PS_CHECK(gate->await(1));
    auto inner = std::async(std::launch::async, [&] {
      return demand.request({{"inner", TileRequest{{{0, point(0, 2)}}}}},
                            stop_inner.token());
    });
    // Both independent runs reach their input callback before cancellation.
    PS_CHECK(gate->await(2));
    if (cancelled == 1)
      stop_outer.cancel();
    if (cancelled == 2)
      stop_inner.cancel();
    gate->allow(2);
    auto inner_result = inner.get();
    gate->open();
    auto outer_result = outer.get();
    if (outer_result.ok() != (cancelled != 1) ||
        inner_result.ok() != (cancelled != 2))
      std::cerr << "shared scenario " << scenario
                << ": outer=" << static_cast<int>(outer_result.status().code)
                << ' ' << outer_result.status().message
                << "; inner=" << static_cast<int>(inner_result.status().code)
                << ' ' << inner_result.status().message << '\n';
    PS_CHECK(outer_result.ok() == (cancelled != 1));
    PS_CHECK(inner_result.ok() == (cancelled != 2));
    if (cancelled == 1)
      PS_CHECK(outer_result.status().code == ErrorCode::Cancelled);
    if (cancelled == 2)
      PS_CHECK(inner_result.status().code == ErrorCode::Cancelled);
    if (outer_result.ok()) {
      double value = 0;
      PS_CHECK(result_input_fixture::read(
                   outer_result.value().results.at("outer"), {0, 0}, &value, 8)
                   .ok() &&
               value == 7);
      PS_CHECK(multi_result::take(
          outer_result.value().dependencies.tile_record({0, 0, 0})));
      PS_CHECK(multi_result::take(
          outer_result.value().dependencies.tile_record({1, 0, 0})));
      PS_CHECK(dirty_tiles(outer_result.value().dependencies, 0, point(0, 2),
                           1) == point(0, 2));
    }
    if (inner_result.ok()) {
      double value = 0;
      PS_CHECK(result_input_fixture::read(
                   inner_result.value().results.at("inner"), {0, 0}, &value, 8)
                   .ok() &&
               value == 7);
      PS_CHECK(context.resource_budget()
                   .value()
                   .statistics()
                   .live[ResourceKind::Payload] >= 8);
      PS_CHECK(multi_result::take(
          inner_result.value().dependencies.tile_record({0, 0, 0})));
      PS_CHECK(!multi_result::take(
          inner_result.value().dependencies.tile_record({1, 0, 0})));
      PS_CHECK(dirty_tiles(inner_result.value().dependencies, 0, point(0, 2),
                           0) == point(0, 2));
    }
    PS_CHECK(gate->entered == (cancelled == 1 ? 2U : 3U));
  }
  return 0;
}
int auxiliary_cancellation() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate, true);
  GraphContext graph(gate_document());
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan;
  ExecutionContext context(registry, {2, false, 8, 4096});
  auto demand =
      context
          .open_demand(
              plan, {{binding(context, "x",
                              values<double>(ElementType::Float64, {7, 9}))}})
          .take_value();
  CancellationSource auxiliary;
  ExecutionOptions options;
  options.dependencies.sets.cancellation = auxiliary.token();
  auto a = std::async(std::launch::async, [&] {
    return demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}}, {},
                          options);
  });
  PS_CHECK(gate->await(1));
  auto b = std::async(std::launch::async, [&] {
    return demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}});
  });
  PS_CHECK(gate->await(2));
  auxiliary.cancel();
  gate->open();
  PS_CHECK(a.get().status().code == ErrorCode::Cancelled);
  auto survived = b.get();
  PS_CHECK(survived.ok());
  PS_CHECK(gate->entered == 2 && gate->active == 0);
  return 0;
}
int impure_ancestor() {
  auto gate = std::make_shared<Gate>();
  auto effects = std::make_shared<std::atomic<unsigned>>(0);
  auto registry = gated_registry(gate, true, effects);
  WorkflowDocument document;
  document.nodes = {{1, "effect", {}, {}},
                    {2, "wait", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"y", 2, "value"}};
  GraphContext graph(document);
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan;
  ExecutionContext context(registry, {2, false, 8, 4096});
  auto demand = context.open_demand(plan, {}).take_value();
  auto a = std::async(std::launch::async, [&] {
    return demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}});
  });
  PS_CHECK(gate->await(1));
  auto b = std::async(std::launch::async, [&] {
    return demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}});
  });
  PS_CHECK(gate->await(2));
  // Both callbacks overlap; serialize publication because the handle
  // permits a Stale result when concurrent structural commits collide.
  gate->allow(1);
  auto first = a.get();
  gate->open();
  auto second = b.get();
  PS_REQUIRE_OK(first);
  PS_REQUIRE_OK(second);
  PS_CHECK(first.ok() && second.ok());
  double x = 0, y = 0;
  PS_CHECK(
      result_input_fixture::read(first.value().results.at("y"), {0, 0}, &x, 8)
          .ok());
  PS_CHECK(
      result_input_fixture::read(second.value().results.at("y"), {0, 0}, &y, 8)
          .ok());
  PS_CHECK(x == 1 && y == 2 && *effects == 2);
  for (const auto* dependencies :
       {&first.value().dependencies, &second.value().dependencies}) {
    PS_CHECK(multi_result::take(dependencies->tile_record({0, 0, 0})));
    PS_CHECK(multi_result::take(dependencies->tile_record({1, 0, 0})));
  }
  return 0;
}
int concurrent_fallback() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate, false, {}, true, true);
  auto doc = gate_document();
  doc.nodes.push_back({2, "wait", {WorkflowNodeOutput{1, "value"}}, {}});
  doc.outputs = {{"y", 2, "value"}};
  GraphContext graph(doc);
  PlanningOptions planning;
  planning.tile_extent = 1;
  planning.execution_mode = ExecutionMode::NativeGpu;
  auto plan = Compiler(registry).compile(graph, planning).take_value().plan;
  ExecutionContext context(registry, {2, false, 8, 4096, 128});
  auto demand =
      context
          .open_demand(
              plan, {{binding(context, "x",
                              values<double>(ElementType::Float64, {7, 9}))}})
          .take_value();
  CancellationSource stop;
  auto owner = std::async(std::launch::async, [&] {
    return demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}},
                          stop.token());
  });
  PS_CHECK(gate->await(1));
  auto other = std::async(std::launch::async, [&] {
    return demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}});
  });
  PS_CHECK(gate->await(2));
  stop.cancel();
  gate->open();
  auto cancelled = owner.get();
  auto completed = other.get();
  double actual = 0;
  PS_CHECK(cancelled.status().code == ErrorCode::Cancelled);
  PS_CHECK(completed.ok() &&
           result_input_fixture::read(completed.value().results.at("y"), {0, 0},
                                      &actual, 8)
               .ok() &&
           actual == 7);
  PS_CHECK(completed.value().diagnostics.selected_backends.at({2, 0}) ==
           Backend::Cpu);
  PS_CHECK(context.cache_statistics().retained_bytes == 0);
  PS_CHECK(demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}})
               .value()
               .diagnostics.cache_hits == 0);
  return 0;
}
struct ProtocolCancellationProbe {
  CancellationSource cancellation;
  std::atomic<unsigned> starts{0}, polls{0}, destroys{0};
  std::atomic<bool> latched_before_cancel{false};
};
struct ProtocolCancelProgram {
  std::shared_ptr<ProtocolCancellationProbe> probe;
  explicit ProtocolCancelProgram(
      std::shared_ptr<ProtocolCancellationProbe> probe)
      : probe(std::move(probe)) {
    ++this->probe->starts;
  }
  ~ProtocolCancelProgram() { ++probe->destroys; }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++probe->polls;
    double value = 0;
    // The static mapper grants no payload. The real read records Protocol
    // first.
    auto failure = phase.read_tensor(0, 0, {0, 0}, &value, sizeof(value));
    probe->latched_before_cancel =
        phase.failure && phase.failure->load() == ErrorCode::InvalidArgument;
    probe->cancellation.cancel();
    return Result<ResultProgramPoll>(std::move(failure));
  }
};
int protocol_first_cause() {
  auto probe = std::make_shared<ProtocolCancellationProbe>();
  auto registry = std::make_shared<OperationRegistry>();
  auto op = copy_definition("wait", sizeof(ProtocolCancelProgram));
  op.map_dependencies = [](const ResultStaticQuery&) {
    return Result<ResultStaticMapping>(ResultStaticMapping{});
  };
  op.start_result = [probe](const ResultProgramQuery&,
                            const BufferAllocator& allocator) {
    return ResultContinuation::make<ProtocolCancelProgram>(allocator, probe);
  };
  multi_result::check(registry->register_operation(std::move(op)));
  multi_result::check(registry->freeze());
  GraphContext graph(gate_document());
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan;
  ExecutionContext context(registry, {1, false, 8, 4096});
  auto demand =
      context
          .open_demand(
              plan, {{binding(context, "x",
                              values<double>(ElementType::Float64, {7, 9}))}})
          .take_value();
  auto failed = demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}},
                               probe->cancellation.token());
  PS_CHECK(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
           failed.status().reason == FailureReason::UnauthorizedRead &&
           failed.status().detail.origin == FailureOrigin::Protocol &&
           failed.status().detail.scope == FailureScope::Group &&
           !failed.status().detail.atom);
  PS_CHECK(probe->latched_before_cancel &&
           probe->cancellation.token().cancelled() && probe->starts == 1 &&
           probe->polls == 1 && probe->destroys == 1);
  PS_CHECK(context.cache_statistics().retained_bytes == 0);
  return 0;
}
int cancelled_run_and_frozen() {
  auto gate = std::make_shared<Gate>();
  gate->ignore_cancellation = true;
  auto registry = gated_registry(gate, true);
  GraphContext graph(gate_document());
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan;
  ExecutionContext context(registry, {2, false, 8, 4096});
  ExecutionBindings bindings{
      {binding(context, "x", values<double>(ElementType::Float64, {7, 9}))}};
  auto demand = context.open_demand(plan, bindings).take_value();
  CancellationSource stop;
  DemandQuery query{{"y", TileRequest{{{0, point(0, 2)}}}}};
  auto p0 = std::async(std::launch::async,
                       [&] { return demand.request(query, stop.token()); });
  PS_CHECK(gate->await(1));
  stop.cancel();
  context.clear_result_cache();
  auto p1 =
      std::async(std::launch::async, [&] { return demand.request(query); });
  PS_CHECK(gate->await(2));
  gate->allow(1);
  PS_CHECK(p0.get().status().code == ErrorCode::Cancelled);
  auto follower =
      std::async(std::launch::async, [&] { return demand.request(query); });
  PS_CHECK(gate->await(3));
  gate->allow(2);
  PS_CHECK(p1.get().ok());
  gate->open();
  auto joined = follower.get();
  PS_CHECK(joined.ok());
  PS_CHECK(gate->entered == 3);
  // Retire the completed caller before exercising a new producer.
  joined = Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
  // A frozen waiter retains the old bundle while latest publication is stale.
  auto frozen = demand.freeze().take_value();
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->entered = 0;
    gate->released = 0;
    gate->release = false;
    gate->ignore_cancellation = false;
  }
  auto latest =
      std::async(std::launch::async, [&] { return demand.request(query); });
  PS_CHECK(gate->await(1));
  auto pinned = std::async(std::launch::async, [&] {
    return context.execute_fragments(frozen, query);
  });
  PS_CHECK(gate->await(2));
  bindings.inputs[0].result =
      binding(context, bindings.inputs[0].name,
              values<double>(ElementType::Float64, {8, 9}))
          .result;
  PS_CHECK(demand.replace_bindings(bindings).ok());
  gate->open();
  PS_CHECK(latest.get().status().code == ErrorCode::Stale);
  auto old = pinned.get();
  double result = 0;
  PS_CHECK(old.ok() &&
           result_input_fixture::read(old.value().results.at("y"), {0, 0},
                                      &result, 8)
               .ok() &&
           result == 7);
  PS_CHECK(gate->entered == 2);
  auto current = demand.request(query);
  PS_CHECK(current.ok() &&
           result_input_fixture::read(current.value().results.at("y"), {0, 0},
                                      &result, 8)
               .ok() &&
           result == 8);
  return 0;
}

int cache_work_and_epoch() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate, true);
  auto document = gate_document();
  for (std::uint64_t i = 2; i <= 64; ++i)
    document.nodes.push_back(
        {i, "wait", {WorkflowNodeOutput{i - 1, "value"}}, {}});
  document.outputs = {{"y", 64, "value"}};
  GraphContext graph(document);
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan;
  ExecutionContextConfig config{1, false, 8, 65536, 32768};
  ResourceLimits resources;
  // This deep graph retains each Result's mandatory structural ancestry.
  resources.capacity[ResourceKind::Metadata] = 32 * 1048576;
  config.managed_resources = resources;
  ExecutionContext context(registry, config);
  auto demand =
      context
          .open_demand(
              plan, {{binding(context, "x",
                              values<double>(ElementType::Float64, {7, 9}))}})
          .take_value();
  gate->open();
  ExecutionOptions options;
  // Keep mandatory traversal of the 64-node staged graph independent of the
  // one-unit optional cache-proof budget exercised below.
  options.maximum_dependency_work = 16 * 1048576;
  options.maximum_dependency_cache_work = 1;
  auto bounded =
      demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}}, {}, options);
  PS_CHECK(bounded.ok() && gate->entered == 64);
  PS_CHECK(bounded.value().diagnostics.dependency_cache_records_visited == 0 &&
           bounded.value().diagnostics.dependency_cache_work == 0);
  PS_CHECK(context.cache_statistics().retained_bytes == 0);
  // A real outstanding producer captured the old epoch. Its successful late
  // completion cannot repopulate a cleared cache, even with a live waiter.
  GraphContext short_graph(gate_document());
  auto short_plan =
      Compiler(registry)
          .compile(short_graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan;
  ExecutionBindings short_bindings{
      {binding(context, "x", values<double>(ElementType::Float64, {7, 9}))}};
  auto short_demand =
      context.open_demand(short_plan, short_bindings).take_value();
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->release = false;
    gate->entered = 0;
  }
  auto pending = std::async(std::launch::async, [&] {
    return short_demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}});
  });
  PS_CHECK(gate->await(1));
  context.clear_result_cache();
  gate->open();
  PS_CHECK(pending.get().ok() &&
           context.cache_statistics().retained_bytes == 0);
  PS_CHECK(short_demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}}).ok());
  PS_CHECK(gate->entered == 2);
  PS_CHECK(short_demand.replace_bindings(short_bindings).ok());
  PS_CHECK(short_demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}}).ok());
  PS_CHECK(gate->entered == 2);
  return 0;
}
int cached_atomic_ancestor() {
  auto gate = std::make_shared<Gate>();
  gate->open();
  auto registry = gated_registry(gate, false, {}, true);
  auto document = gate_document();
  document.nodes.push_back({2, "wait", {WorkflowNodeOutput{1, "value"}}, {}});
  document.outputs = {{"y", 2, "value"}, {"ancestor", 1, "value"}};
  GraphContext graph(document);
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan;
  ExecutionContext context(registry, {1, false, 8, 4096, 512});
  ExecutionBindings bindings{
      {binding(context, "x", values<double>(ElementType::Float64, {7, 9}))}};
  auto demand = context.open_demand(plan, bindings).take_value();
  auto first = demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}});
  PS_CHECK(first.ok() && gate->entered == 2);
  auto reused = demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}});
  PS_CHECK(reused.ok() && gate->entered == 2 &&
           reused.value().diagnostics.operation_timings.empty());
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto warm = demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}});
  PS_CHECK(warm.ok() && gate->entered == 2);
  PS_CHECK(warm.value().diagnostics.operation_timings.empty());
  PS_CHECK(multi_result::take(warm.value().dependencies.tile_record({0, 0, 0}))
               ->inputs[0]
               .tiles == all_tiles(2));
  double value = 0;
  PS_CHECK(result_input_fixture::read(warm.value().results.at("y"), {0, 0},
                                      &value, 8)
               .ok() &&
           value == 7);
  // The root's 16-byte result survives while its atomic ancestor's pixels
  // are evicted. Importing its evidence must not block a later actual read.
  document.outputs = {{"a_desc", 2, "value"}, {"z_ancestor", 1, "value"}};
  GraphContext named_graph(document);
  auto named_plan =
      Compiler(registry)
          .compile(named_graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan;
  ExecutionContext tight(registry, {1, false, 8, 4096, 16});
  ExecutionBindings tight_bindings{
      {binding(tight, "x", values<double>(ElementType::Float64, {7, 9}))}};
  auto query = tight.open_demand(named_plan, tight_bindings).take_value();
  PS_CHECK(query.request({{"a_desc", TileRequest{{{0, point(0, 2)}}}}}).ok());
  // A -> B -> C: the 16-byte LRU retains C and evicts B pixels. The
  // active C subscription must still transpose an A edit through B evidence.
  const auto old_bundle = query.freeze().take_value();
  tight_bindings.inputs[0] =
      binding(tight, "x", values<double>(ElementType::Float64, {11, 9}));
  auto change = query.replace_bindings(tight_bindings);
  PS_CHECK(change.ok() &&
           change.value().potential_dirty.at("a_desc").members.at(0) ==
               point(0, 2));
  auto updated = query.request({{"a_desc", TileRequest{{{0, point(0, 2)}}}}});
  PS_CHECK(updated.ok() &&
           result_input_fixture::read(updated.value().results.at("a_desc"),
                                      {0, 0}, &value, 8)
               .ok() &&
           value == 11);
  auto pinned = tight.execute_fragments(
      old_bundle, {{"a_desc", TileRequest{{{0, point(0, 2)}}}}});
  PS_CHECK(pinned.ok() &&
           result_input_fixture::read(pinned.value().results.at("a_desc"),
                                      {0, 0}, &value, 8)
               .ok() &&
           value == 7);
  // Restore the current descendant to the single-entry LRU before asking for
  // both nodes; the old frozen computation must not replace current evidence.
  PS_CHECK(query.replace_bindings(tight_bindings).ok());
  PS_CHECK(query.request({{"a_desc", TileRequest{{{0, point(0, 2)}}}}}).ok());
  const auto previous = gate->entered;
  PS_CHECK(query.replace_bindings(tight_bindings).ok());
  auto both = query.request({{"a_desc", TileRequest{{{0, point(0, 2)}}}},
                             {"z_ancestor", TileRequest{{{0, point(0, 2)}}}}});
  // Static execution retains B before C looks up its cache entry. The 16-byte
  // LRU evicts C when admitting B, so both callbacks run.
  PS_CHECK(both.ok() && gate->entered == previous + 2);
  PS_CHECK(result_input_fixture::read(both.value().results.at("z_ancestor"),
                                      {0, 1}, &value, 8)
               .ok() &&
           value == 9);
  PS_CHECK(multi_result::take(both.value().dependencies.tile_record({0, 0, 0}))
               ->inputs[0]
               .tiles == all_tiles(2));
  // Clearing during B invalidates this Run's captured cache epoch. Later B/C
  // retention must not refill it. Restore C, then block the scheduled B call.
  PS_CHECK(query.replace_bindings(tight_bindings).ok());
  PS_CHECK(query.request({{"a_desc", TileRequest{{{0, point(0, 2)}}}}}).ok());
  PS_CHECK(tight.cache_statistics().retained_bytes == 16);
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->entered = 0;
    gate->released = 0;
    gate->release = false;
  }
  PS_CHECK(query.replace_bindings(tight_bindings).ok());
  const auto starts_before = gate->wide_starts.load();
  auto verifying = std::async(std::launch::async, [&] {
    return query.request({{"a_desc", TileRequest{{{0, point(0, 2)}}}}});
  });
  PS_CHECK(gate->await(1));
  // Only B has entered its factory; C's instruction and cache lookup are
  // pending.
  PS_CHECK(gate->wide_starts == starts_before + 1);
  tight.clear_result_cache();
  PS_CHECK(tight.cache_statistics().retained_bytes == 0);
  gate->open();
  auto cleared = verifying.get();
  PS_CHECK(cleared.ok() && gate->entered == 2 &&
           gate->wide_starts == starts_before + 2 &&
           tight.cache_statistics().retained_bytes == 0);
  PS_CHECK(result_input_fixture::read(cleared.value().results.at("a_desc"),
                                      {0, 0}, &value, 8)
               .ok() &&
           value == 11);
  PS_CHECK(result_input_fixture::read(cleared.value().results.at("a_desc"),
                                      {0, 1}, &value, 8)
               .ok() &&
           value == 9);
  PS_CHECK(
      multi_result::take(cleared.value().dependencies.tile_record({0, 0, 0}))
          ->inputs[0]
          .tiles == all_tiles(2));
  return 0;
}
int cache_snapshot_bits() {
  auto registry = snapshot_registry();
  for (auto type : {ElementType::UInt8, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64}) {
    const auto width = TensorSpan::element_size(type);
    std::vector<std::uint8_t> bytes(width * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = static_cast<std::uint8_t>(255 - i);
    auto reversed = TensorSpan::create({type, {1, 3}}, Region::whole({1, 3}),
                                       {width * 2,
                                        {static_cast<std::int64_t>(width * 3),
                                         -static_cast<std::int64_t>(width)}},
                                       bytes)
                        .take_value();
    auto writer = TensorWriter::allocate({type, {1, 3}}, Region::whole({1, 3}),
                                         BufferAllocator{})
                      .take_value();
    for (std::size_t i = 0; i < 3; ++i)
      std::memcpy(writer.data() + i * width, bytes.data() + (2 - i) * width,
                  width);
    auto packed = std::move(writer).publish().take_value();
    auto document = identity_document(packed);
    document.nodes[0].operation = "test.snapshot_identity";
    GraphContext graph(document);
    auto plan =
        Compiler(registry)
            .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
            .take_value()
            .plan;
    ExecutionContext context(registry, {1, false, 8, 4096, 512});
    auto demand =
        context.open_demand(plan, {{binding(context, "image", packed)}})
            .take_value();
    const auto q =
        point(0, 3).unite(point(2, 3), ResourceBudget{}).take_value();
    auto initial = demand.request({{"result", TileRequest{{{0, q}}}}});
    PS_CHECK(initial.ok() && initial.value().diagnostics.cache_hits == 0);
    InputSnapshotStore store({1024, 1});
    auto snapshot = std::make_shared<const InputSnapshot>(
        store
            .import_tensor(Tensor::create(reversed.descriptor(),
                                          Footprint::from_regions(
                                              reversed.descriptor().shape,
                                              {reversed.region()})
                                              .take_value(),
                                          {reversed})
                               .take_value())
            .take_value());
    auto edited = demand.replace_bindings(
        {{snapshot_binding(context, "image", *snapshot, packed)}});
    PS_CHECK(edited.ok() &&
             edited.value().potential_dirty.at("result").empty());
    auto same = demand.request({{"result", TileRequest{{{0, q}}}}});
    PS_CHECK(same.ok() && same.value().diagnostics.cache_hits > 0);
    for (auto at : {0U, 2U}) {
      std::uint8_t observed[8]{};
      PS_CHECK(result_input_fixture::read(same.value().results.at("result"),
                                          {0, at}, observed, width)
                   .ok());
      PS_CHECK(std::memcmp(observed, bytes.data() + (2 - at) * width, width) ==
               0);
    }
  }
  return 0;
}
std::shared_ptr<OperationRegistry> routing_cache_fixture_registry() {
  return ps::test::conditional_sum_registry();
}
WorkflowDocument routing_cache_fixture_document() {
  return routing_document();
}
int content_cache() {
  auto registry = routing_cache_fixture_registry();
  auto document = routing_cache_fixture_document();
  GraphContext graph(document);
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan;
  ExecutionContextConfig config{1, false, 8, 4096};
  config.result_cache_bytes = 2048;
  ExecutionContext context(registry, config);
  ExecutionBindings bindings{
      {binding(context, "data",
               values<double>(ElementType::Float64, {1, 2, 3, 0, 5})),
       binding(context, "control",
               values<std::int64_t>(ElementType::Int64, {0, 0, 0, 0, 0}))}};
  auto demand = context.open_demand(plan, bindings).take_value();
  const auto q = point(0).unite(point(4), ResourceBudget{}).take_value();
  DemandQuery query{{"sum", TileRequest{{{0, q}}}}};
  auto first = demand.request(query);
  PS_CHECK(first.ok() && first.value().diagnostics.cache_hits == 0);
  auto frozen = context.freeze(plan, bindings).take_value();
  auto reused = demand.request(query);
  PS_CHECK(reused.ok() && reused.value().diagnostics.cache_hits > 0 &&
           reused.value().diagnostics.operation_timings.empty());
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto warm = demand.request(query);
  PS_CHECK(warm.ok() && warm.value().diagnostics.cache_hits > 0 &&
           warm.value().diagnostics.operation_timings.empty());
  bindings.inputs[0].result =
      binding(context, bindings.inputs[0].name,
              values<double>(ElementType::Float64, {1, 2, 777, 0, 5}))
          .result;
  PS_CHECK(demand.replace_bindings(bindings)
               .value()
               .potential_dirty.at("sum")
               .empty());
  auto unchanged = demand.request(query);
  PS_CHECK(unchanged.ok() && unchanged.value().diagnostics.cache_hits > 0);
  // Cached and freshly computed rows must merge under the current identity.
  auto mixed = demand.request(
      {{"sum",
        TileRequest{
            {{0, point(0).unite(point(1), ResourceBudget{}).take_value()}}}}});
  PS_CHECK(mixed.ok() && mixed.value().diagnostics.cache_hits > 0);
  bindings.inputs[1].result =
      binding(context, bindings.inputs[1].name,
              values<std::int64_t>(ElementType::Int64, {0, 0, 0, 1, 0}))
          .result;
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto changed_relation = demand.request(query);
  PS_CHECK(changed_relation.ok() &&
           changed_relation.value().diagnostics.cache_hits == 0);
  double answer = 0;
  PS_CHECK(result_input_fixture::read(
               changed_relation.value().results.at("sum"), {0, 0}, &answer, 8)
               .ok() &&
           answer == 1);
  PS_CHECK(dirty_tiles(changed_relation.value().dependencies, 0, point(3), 0) ==
           q);
  PS_CHECK(demand.replace_bindings(bindings).ok());
  PS_CHECK(demand.request(query).value().diagnostics.cache_hits > 0);
  auto old = context.execute_fragments(frozen, query);
  PS_CHECK(old.ok() && old.value().diagnostics.cache_hits > 0);
  PS_CHECK(dirty_tiles(old.value().dependencies, 0, point(3), 0) == q);
  bindings.inputs[0].result =
      binding(context, bindings.inputs[0].name,
              values<double>(ElementType::Float64, {1, 2, 777, 9, 5}))
          .result;
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto latest = demand.request(query);
  PS_CHECK(latest.ok() && latest.value().diagnostics.cache_hits == 0);
  PS_CHECK(result_input_fixture::read(latest.value().results.at("sum"), {0, 4},
                                      &answer, 8)
               .ok() &&
           answer == 14);
  context.clear_result_cache();
  PS_CHECK(context.cache_statistics().retained_bytes == 0);
  PS_CHECK(dirty_tiles(latest.value().dependencies, 0, point(3), 0) == q);
  PS_CHECK(demand.request(query).value().diagnostics.cache_hits == 0);
  config.result_cache_bytes = 16;
  ExecutionContext small(registry, config);
  ExecutionBindings small_bindings{
      {binding(small, "data",
               values<double>(ElementType::Float64, {1, 2, 777, 9, 5})),
       binding(small, "control",
               values<std::int64_t>(ElementType::Int64, {0, 0, 0, 1, 0}))}};
  auto limited = small.open_demand(plan, small_bindings).take_value();
  PS_CHECK(limited.request({{"sum", TileRequest{{{0, point(0)}}}}}).ok());
  PS_CHECK(limited.request({{"sum", TileRequest{{{0, point(1)}}}}}).ok());
  PS_CHECK(limited.request({{"sum", TileRequest{{{0, point(2)}}}}}).ok());
  auto all = limited.request({{"sum", TileRequest{{{0, all_tiles(5)}}}}});
  PS_CHECK(all.ok() && small.cache_statistics().evictions > 0);
  PS_CHECK(dirty_tiles(all.value().dependencies, 1, point(3), 0) ==
           all_tiles(5));
  bindings.inputs[0].result =
      binding(context, bindings.inputs[0].name,
              values<double>(ElementType::Float64, {1, 2, 777, 10, 5}))
          .result;
  small_bindings.inputs[0] = binding(
      small, "data", values<double>(ElementType::Float64, {1, 2, 777, 10, 5}));
  auto edit = limited.replace_bindings(small_bindings);
  PS_CHECK(edit.ok() && edit.value().potential_dirty.at("sum").members.at(0) ==
                            all_tiles(5));
  return 0;
}
int structured_content_cache() {
  auto registry = ps::test::conditional_sum_registry();
  GraphContext graph(routing_document());
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan;
  ExecutionContextConfig config{1, false, 8, 4096};
  config.result_cache_bytes = 2048;
  ExecutionContext context(registry, config);
  const auto root = context.resource_budget().take_value();
  const auto data = [&](std::vector<double> numbers) {
    return source(root, values<double>(ElementType::Float64, numbers));
  };
  const auto control = [&](std::vector<std::int64_t> numbers) {
    return source(root, values<std::int64_t>(ElementType::Int64, numbers));
  };
  ExecutionBindings bindings{
      {{"data", data({1, 2, 3, 0, 5})}, {"control", control({0, 0, 0, 0, 0})}}};
  auto demand = context.open_demand(plan, bindings).take_value();
  const auto q = point(0).unite(point(4), ResourceBudget{}).take_value();
  DemandQuery query{{"sum", TileRequest{{{0, q}}}}};
  std::uint64_t first_id = 0;
  const void* first_storage = nullptr;
  {
    auto first = demand.request(query);
    PS_CHECK(first.ok() && first.value().diagnostics.cache_hits == 0);
    const auto& output = first.value().results.at("sum");
    first_id = output.object_id();
    auto window = output
                      .acquire_tensor(output.descriptor().take_value(), 0,
                                      Region({{0, 1}, {0, 1}}))
                      .take_value();
    first_storage = window.storage_owner_token();
  }
  PS_CHECK(context.cache_statistics().entries == 2 &&
           context.cache_statistics().retained_bytes > 0);
  bindings.inputs[0].result = data({1, 2, 777, 0, 5});
  bindings.inputs[1].result = control({0, 0, 0, 0, 0});
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto warm = demand.request(query);
  if (!warm.ok())
    std::cerr << warm.status().message << '\n';
  PS_CHECK(warm.ok() && warm.value().diagnostics.cache_hits > 0 &&
           warm.value().diagnostics.operation_timings.empty());
  auto held = warm.value().results.at("sum");
  PS_CHECK(held.object_id() != first_id);
  auto window = held.acquire_tensor(held.descriptor().take_value(), 0,
                                    Region({{0, 1}, {0, 1}}))
                    .take_value();
  PS_CHECK(window.storage_owner_token() == first_storage);
  double number = 0;
  PS_CHECK(result_input_fixture::read(held, {0, 4}, &number, 8).ok() &&
           number == 5);
  PS_CHECK(dirty_tiles(warm.value().dependencies, 0, point(2), 0).empty());
  bindings.inputs[1].result = control({0, 0, 0, 1, 0});
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto routed = demand.request(query);
  PS_CHECK(routed.ok() && routed.value().diagnostics.cache_hits == 0 &&
           dirty_tiles(routed.value().dependencies, 0, point(3), 0) == q);
  bindings.inputs[0].result = data({1, 2, 777, 9, 5});
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto changed = demand.request(query);
  PS_CHECK(changed.ok() && changed.value().diagnostics.cache_hits == 0 &&
           result_input_fixture::read(changed.value().results.at("sum"), {0, 4},
                                      &number, 8)
               .ok() &&
           number == 14);
  context.clear_result_cache();
  PS_CHECK(context.cache_statistics().retained_bytes == 0 &&
           context.cache_statistics().entries == 0);
  PS_CHECK(result_input_fixture::read(held, {0, 4}, &number, 8).ok() &&
           number == 5);
  bindings.inputs[0].result = data({1, 2, 777, 9, 5});
  bindings.inputs[1].result = control({0, 0, 0, 1, 0});
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto after_clear = demand.request(query);
  PS_CHECK(after_clear.ok() && after_clear.value().diagnostics.cache_hits == 0);
  context.clear_result_cache();
  bindings.inputs[0].result = data({1, 2, 777, 9, 5});
  PS_CHECK(demand.replace_bindings(bindings).ok());
  ExecutionOptions bounded;
  bounded.maximum_dependency_cache_work = 1;
  auto low_work = demand.request(query, {}, bounded);
  PS_CHECK(low_work.ok() && low_work.value().diagnostics.cache_hits == 0 &&
           context.cache_statistics().retained_bytes == 0);
  {
    auto fresh = context.freeze(plan, bindings).take_value();
    auto seed = context.execute_fragments(fresh, query);
    PS_CHECK(seed.ok());
  }
  for (std::uint64_t fuel : {0U, 8U, 64U, 128U, 256U, 512U, 1024U}) {
    auto fresh = context.freeze(plan, bindings).take_value();
    bounded.maximum_dependency_cache_work = fuel;
    auto completed = context.execute_fragments(fresh, query, {}, bounded);
    PS_CHECK(completed.ok() &&
             result_input_fixture::read(completed.value().results.at("sum"),
                                        {0, 4}, &number, 8)
                 .ok() &&
             number == 14);
  }
  bounded.maximum_dependency_cache_work = 0;
  // Compare optional proof budgets on one frozen identity. Creating another
  // owner changes mandatory identity/capture work and invalidates the measured
  // minimal Run budget before the cache path is exercised.
  auto budget_owner = context.freeze(plan, bindings);
  PS_REQUIRE_OK(budget_owner);
  const auto budget_frozen = budget_owner.take_value();
  std::uint64_t lower = 0, upper = 4096;
  while (lower + 1 < upper) {
    const auto middle = lower + (upper - lower) / 2;
    bounded.maximum_dependency_work = middle;
    auto attempted =
        context.execute_fragments(budget_frozen, query, {}, bounded);
    if (attempted.ok()) {
      upper = middle;
    } else {
      PS_CHECK(attempted.status().code == ErrorCode::ResourceExhausted);
      lower = middle;
    }
  }
  bounded.maximum_dependency_work = upper;
  for (std::uint64_t fuel : {8U, 64U, 128U, 256U, 512U, 1024U}) {
    bounded.maximum_dependency_cache_work = fuel;
    auto completed =
        context.execute_fragments(budget_frozen, query, {}, bounded);
    if (!completed.ok())
      std::cerr << "cache fuel=" << fuel << " Run fuel=" << upper << ' '
                << completed.status().message << '\n';
    PS_CHECK(completed.ok());
  }
  {
    auto aggregate_config = config;
    aggregate_config.maximum_dependency_cache_metadata = 4096;
    ExecutionContext aggregate(registry, aggregate_config);
    const auto aggregate_root = aggregate.resource_budget().take_value();
    for (unsigned seed = 0; seed < 32; ++seed) {
      ExecutionBindings current{
          {{"data",
            source(aggregate_root, values<double>(ElementType::Float64,
                                                  {seed + 1.0, 2, 3, 0, 5}))},
           {"control",
            source(aggregate_root, values<std::int64_t>(ElementType::Int64,
                                                        {0, 0, 0, 0, 0}))}}};
      auto frozen = aggregate.freeze(plan, current).take_value();
      auto output = aggregate.execute_fragments(frozen, query);
      PS_CHECK(output.ok());
    }
    const auto stats = aggregate.cache_statistics();
    PS_CHECK(stats.entries > 0 && stats.entries < 32 && stats.evictions > 0 &&
             stats.retained_bytes < aggregate_config.result_cache_bytes);
  }
  return 0;
}
int concurrent_failure_isolation() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate, true);
  GraphContext graph(gate_document());
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan;
  ExecutionContext context(registry, {3, false, 8, 4096});
  ExecutionBindings bindings{
      {binding(context, "x",
               values<double>(ElementType::Float64,
                              {1, std::numeric_limits<double>::infinity()}))}};
  auto demand = context.open_demand(plan, bindings).take_value();
  const DemandQuery wide{{"y", TileRequest{{{0, all_tiles(2)}}}}};
  auto producer =
      std::async(std::launch::async, [&] { return demand.request(wide); });
  PS_CHECK(gate->await(1));
  auto follower =
      std::async(std::launch::async, [&] { return demand.request(wide); });
  PS_CHECK(gate->await(2));
  auto narrow = std::async(std::launch::async, [&] {
    return demand.request({{"y", TileRequest{{{0, point(0, 2)}}}}});
  });
  PS_CHECK(gate->await(3));
  gate->open();
  auto failed = producer.get(), shared_failure = follower.get();
  auto survived = narrow.get();
  PS_CHECK(shared_failure.status().code == failed.status().code &&
           shared_failure.status().message == failed.status().message &&
           shared_failure.status().reason == failed.status().reason);
  PS_CHECK(failed.status().code == ErrorCode::OperationFailed &&
           failed.status().message == "nonfinite sample 1");
  double value = 0;
  PS_CHECK(survived.ok() &&
           result_input_fixture::read(survived.value().results.at("y"), {0, 0},
                                      &value, 8)
               .ok() &&
           value == 1);
  PS_CHECK(gate->entered == 5 && gate->narrow_starts == 5 &&
           gate->wide_starts == 0);
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->entered = 0;
    gate->release = false;
  }
  bindings.inputs[0].result =
      binding(context, bindings.inputs[0].name,
              values<double>(ElementType::Float64,
                             {std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::infinity()}))
          .result;
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto later_atom = std::async(std::launch::async, [&] {
    return demand.request({{"y", TileRequest{{{0, point(1, 2)}}}}});
  });
  PS_CHECK(gate->await(1));
  auto ordered = std::async(std::launch::async, [&] {
    return demand.request({{"y", TileRequest{{{0, all_tiles(2)}}}}});
  });
  PS_CHECK(gate->await(2));
  gate->allow(1);
  PS_CHECK(later_atom.get().status().message == "nonfinite sample 1");
  gate->open();
  PS_CHECK(ordered.get().status().message == "nonfinite sample 0");
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(combined_tokens() == 0);
  PS_CHECK(generations() == 0);
  PS_CHECK(edit_and_cancel() == 0);
  PS_CHECK(replacement_during_shutdown() == 0);
  PS_CHECK(isolation_and_limits() == 0);
  PS_CHECK(snapshot_replacement() == 0);
  PS_CHECK(concurrent_ancestors() == 0);
  PS_CHECK(concurrent_failure_isolation() == 0);
  PS_CHECK(auxiliary_cancellation() == 0);
  PS_CHECK(impure_ancestor() == 0);
  PS_CHECK(concurrent_fallback() == 0);
  PS_CHECK(protocol_first_cause() == 0);
  PS_CHECK(content_cache() == 0);
  PS_CHECK(structured_content_cache() == 0);
  PS_CHECK(cache_work_and_epoch() == 0);
  PS_CHECK(cache_snapshot_bits() == 0);
  PS_CHECK(cancelled_run_and_frozen() == 0);
  PS_CHECK(cached_atomic_ancestor() == 0);
  return 0;
}
