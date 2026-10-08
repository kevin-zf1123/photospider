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

#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
Footprint point(std::uint64_t at, std::uint64_t size = 5) {
  return Footprint::from_regions({size}, {Region({{at, 1}})}).take_value();
}
template <class T>
Value values(ElementType type, const std::vector<T>& numbers) {
  auto writer =
      MutableValue::allocate({type, {numbers.size()}},
                             Region::whole({numbers.size()}), BufferAllocator{})
          .take_value();
  std::memcpy(writer.data(), numbers.data(), writer.size());
  return std::move(writer).publish().take_value();
}
ResultRef source(const ResourceBudget& root, const Value& backing) {
  // Immutable caller backing stays in Root Referenced accounting.
  const auto schema = numeric_result_fixture::source_schema(backing);
  auto builder =
      multi_result::take(ResultBuilder::start(root, schema, "test.source"));
  multi_result::check(builder.bind_descriptor_relation(
      multi_result::take(ResultRelation::cartesian(root, 1, {}))));
  multi_result::check(builder.publish_tensor(
      0, backing.region(), backing.layout(),
      multi_result::take(root.reference(backing.storage())),
      multi_result::take(ResultRelation::cartesian(
          root, multi_result::take(schema.tensors[0].sample_count()), {})),
      {true, true, true, true}));
  return multi_result::take(builder.seal());
}
WorkflowDocument scatter_document() {
  WorkflowDocument document;
  numeric_result_fixture::declare_sources(
      &document, {values<double>(ElementType::Float64, {1, 2, 3, 0, 5}),
                  values<std::int64_t>(ElementType::Int64, {0, 0, 0, 0, 0})});
  document.inputs[0].name = "data";
  document.inputs[1].name = "radius";
  document.nodes = {{1,
                     "numeric.radius_scatter",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"sum", 1, "value"}};
  return document;
}
int generations() {
  auto registry = make_default_operation_registry();
  GraphContext graph(scatter_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {1, false, 8, 4096});
  const auto root = context.resource_budget().take_value();
  ExecutionBindings bindings{
      {{"data",
        source(root, values<double>(ElementType::Float64, {1, 2, 3, 0, 5}))},
       {"radius", source(root, values<std::int64_t>(ElementType::Int64,
                                                    {0, 0, 0, 0, 0}))}}};
  auto opened = context.open_demand(plan, bindings);
  PS_CHECK(opened.ok());
  auto demand = opened.take_value();
  PS_CHECK(demand.generation().value() == 1);
  const auto q = point(0).unite(point(4)).take_value();
  DemandQuery query{{"sum", q}};
  auto before = demand.request(query);
  if (!before.ok())
    std::cerr << before.status().message << '\n';
  PS_CHECK(before.ok() && before.value().generation == 1);
  PS_CHECK(before.value()
               .results.at("sum")
               .descriptor()
               .take_value()
               .tensor_coverage(0) == q);
  double result = 0;
  PS_CHECK(numeric_result_fixture::read(before.value().results.at("sum"), {0},
                                        &result, 8)
               .ok() &&
           result == 1);
  PS_CHECK(!numeric_result_fixture::read(before.value().results.at("sum"), {1},
                                         &result, 8)
                .ok());
  auto retained = demand.request(query);
  PS_CHECK(retained.ok() &&
           retained.value().diagnostics.operation_timings.empty() &&
           retained.value().results.at("sum").object_id() ==
               before.value().results.at("sum").object_id());
  auto frozen = demand.freeze().take_value();
  bindings.inputs[1].result =
      source(root, values<std::int64_t>(ElementType::Int64, {0, 0, 0, 3, 0}));
  auto edit = demand.replace_bindings(bindings);
  if (!edit.ok())
    std::cerr << edit.status().message << '\n';
  PS_CHECK(edit.ok() && edit.value().generation == 2 &&
           edit.value().coverage.at("sum") == q);
  PS_CHECK(edit.value().potential_dirty.at("sum") == q);
  auto same_value = demand.request(query);
  PS_CHECK(same_value.ok() &&
           numeric_result_fixture::read(same_value.value().results.at("sum"),
                                        {0}, &result, 8)
               .ok() &&
           result == 1);
  PS_CHECK(same_value.value()
               .dependencies.potential_dirty("data", point(3))
               .value()
               .at("sum") == q);
  auto old = context.execute_fragments(frozen, query);
  PS_CHECK(old.ok() && old.value().generation == 0);
  PS_CHECK(old.value()
               .dependencies.potential_dirty("data", point(3))
               .value()
               .at("sum")
               .empty());
  bindings.inputs[0].result =
      source(root, values<double>(ElementType::Float64, {1, 2, 3, 9, 5}));
  auto changed = demand.replace_bindings(bindings);
  PS_CHECK(changed.ok() && changed.value().potential_dirty.at("sum") == q);
  auto fresh = demand.request(query);
  PS_CHECK(fresh.ok() &&
           numeric_result_fixture::read(fresh.value().results.at("sum"), {4},
                                        &result, 8)
               .ok() &&
           result == 14);
  // Only current witness samples are compared: radius[0..4] and data{0,3,4}.
  bindings.inputs[0].result =
      source(root, values<double>(ElementType::Float64, {1, 2, 777, 9, 5}));
  auto unrelated =
      demand.replace_bindings(bindings, SnapshotAccessOptions{10, {}});
  PS_CHECK(unrelated.ok() &&
           unrelated.value().potential_dirty.at("sum").empty());
  auto failed = demand.replace_bindings(bindings, SnapshotAccessOptions{9, {}});
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
  auto empty = demand.request({{"sum", Footprint::none({5}).take_value()}});
  PS_CHECK(empty.ok() && empty.value()
                             .results.at("sum")
                             .descriptor()
                             .take_value()
                             .tensor_coverage(0)
                             .empty());
  PS_CHECK(
      empty.value().dependencies.source_observations().take_value().empty());
  PS_CHECK(empty.value().dependencies.coverage().at("sum").empty());
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
  std::atomic<unsigned> active{0}, joint_starts{0}, joint_polls{0};
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
  auto schema = multi_result::schema(type, {count});
  schema.id = "manual.lowpass.input";
  schema.tensors[0].key = "data";
  return schema;
}
ExecutionBinding binding(const ExecutionContext& context, std::string name,
                         const Value& backing) {
  return {std::move(name),
          source(multi_result::take(context.resource_budget()), backing)};
}
ExecutionBinding snapshot_binding(const ExecutionContext& context,
                                  std::string name,
                                  const InputSnapshot& snapshot,
                                  const Value& prototype) {
  std::vector<std::uint8_t> bytes(prototype.bytes().size());
  multi_result::check(
      snapshot.read(prototype.region(), bytes.data(), bytes.size()));
  auto backing = multi_result::take(
      Value::create(prototype.descriptor(), prototype.region(),
                    prototype.layout(), std::move(bytes)));
  return binding(context, std::move(name), backing);
}
WorkflowDocument identity_document(const Value& value) {
  WorkflowDocument document;
  numeric_result_fixture::declare_sources(&document, {value});
  document.inputs[0].name = "image";
  document.nodes = {
      {1, "test.snapshot_identity", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"result", 1, "value"}};
  return document;
}
struct GatedProgram {
  std::shared_ptr<Gate> gate;
  std::shared_ptr<std::atomic<unsigned>> effects;
  bool terminal = false, whole = false, staged = false;
  unsigned stage = 0;
  GatedProgram(std::shared_ptr<Gate> gate, bool terminal, bool whole,
               bool staged, std::shared_ptr<std::atomic<unsigned>> effects = {})
      : gate(std::move(gate)),
        effects(std::move(effects)),
        terminal(terminal),
        whole(whole),
        staged(staged) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    const auto& schema = *phase.query.output.result_schema;
    const auto shape = schema.tensors[0].sample_shape();
    const auto outputs = phase.query.tensor_outputs.value_or(
        multi_result::take(Footprint::all(shape)));
    const auto wanted = terminal ? point(0, shape[0])
                        : whole  ? multi_result::take(Footprint::all(shape))
                                 : outputs;
    if (!effects && !outputs.empty() && stage < (staged ? 2U : 1U)) {
      const auto roles = staged && stage == 0 ? 8U : 1U;
      ++stage;
      return Answer(ResultProgramNeed{{}, {}, {{0, 0, wanted, roles}}});
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
    auto builder = multi_result::take(ResultBuilder::start(
        phase.resources, schema, phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{}));
    multi_result::check(builder.bind_descriptor_relation(
        multi_result::take(ResultRelation::cartesian(phase.resources, 1, {}))));
    const double effect = effects ? ++*effects : 0;
    for (const auto& box : outputs.boxes()) {
      auto bytes = multi_result::take(phase.allocator.allocate(
          multi_result::take(box.element_count()) * 8));
      for (std::uint64_t i = 0; i < box.dimensions()[0].extent; ++i) {
        const auto at = box.dimensions()[0].offset + i;
        double value = effect;
        if (!effects) {
          auto status = phase.read_tensor(0, 0, {terminal ? 0 : at}, &value, 8);
          if (!status.ok())
            return Answer(status);
          if (!std::isfinite(value))
            return Answer(Status{ErrorCode::OperationFailed,
                                 "nonfinite sample " + std::to_string(at)});
          if (terminal)
            value += multi_result::take(outputs.element_count());
        }
        std::memcpy(bytes.data() + i * 8, &value, 8);
      }
      auto relation =
          effects ? multi_result::take(ResultRelation::cartesian(
                        phase.resources, shape[0], {}))
          : whole ? multi_result::take(ResultRelation::cartesian(
                        phase.resources, shape[0],
                        {0, 1, 0, shape[0], ResultSupportTarget::Tensor, 0}))
                  : multi_result::take(ResultRelation::mapped(
                        phase.resources, shape, box, shape,
                        terminal ? std::vector<ResultMappedAxis>{{-1, 0, 0, 1}}
                                 : std::vector<ResultMappedAxis>{{0, 0, 1, 1}},
                        {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
      multi_result::check(
          builder.publish_tensor(0, box, {0, {8}, {box.dimensions()[0].offset}},
                                 std::move(bytes).freeze(), std::move(relation),
                                 {true, true, true, true}));
    }
    return Answer(ResultPublication{multi_result::take(builder.seal()), true});
  }
};
struct GatedJoint {
  std::shared_ptr<Gate> gate;
  std::array<GatedProgram, 2> states;
  explicit GatedJoint(const std::shared_ptr<Gate>& gate)
      : gate(gate),
        states{GatedProgram(gate, false, false, false),
               GatedProgram(gate, false, false, false)} {
    ++this->gate->joint_starts;
  }
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    ++gate->joint_polls;
    ResourceVector<ResultJointOutcome> outcomes;
    for (const auto* member : phase.members) {
      const auto key = multi_result::take(result_atom_key(member->query));
      auto result = states.at(key.coordinate.at(0)).poll(*member);
      if (!result.ok() && result.status().code == ErrorCode::OperationFailed &&
          result.status().message.find("nonfinite sample ") == 0) {
        result = Result<ResultProgramPoll>(
            Status{ErrorCode::OperationFailed,
                   result.status().message,
                   FailureReason::InvalidDomain,
                   {FailureOrigin::Domain, FailureScope::Atom, key}});
      }
      outcomes.push_back({key, std::move(result)});
    }
    return Result<ResourceVector<ResultJointOutcome>>(std::move(outcomes));
  }
};
OperationDefinition copy_definition(const std::string& key,
                                    std::uint64_t state) {
  OperationDefinition op;
  op.key = key;
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);
  op.traits.input_schema[0].kind = OperationPortKind::Result;
  op.traits.input_schema[0].rank = 1;
  op.traits.outputs[0] = multi_result::output("value", tensor_schema());
  op.traits.outputs[0].continuation_bytes = state;
  op.traits.outputs[0].maximum_dependency_stages = 4;
  op.traits.workspace_bytes = 256;
  op.traits.requires_metadata_specialization = true;
  op.traits.outputs[0].output_schema.result_schema_id.clear();
  op.traits.outputs[0].output_schema.result_schema_version = 0;
  op.traits.outputs[0].output_schema.rank = 1;
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
    std::shared_ptr<std::atomic<unsigned>> effects = {}, bool terminal = false,
    bool whole = false, bool gpu_fallback = false, bool per_atom = false) {
  auto registry = std::make_shared<OperationRegistry>();
  auto op = copy_definition("wait", sizeof(GatedProgram));
  op.traits.supports_gpu = gpu_fallback;
  op.traits.allows_cpu_fallback = gpu_fallback;
  op.traits.outputs[0].region_rule =
      whole ? OperationRegionRule::Whole : OperationRegionRule::Dependency;
  if (terminal)
    op.traits.outputs[0].observation_kind = ObservationKind::RequestRecord;
  op.start_result = [gate, terminal, whole, staged](
                        const ResultProgramQuery& query,
                        const BufferAllocator& allocator) {
    if (gate && query.tensor_outputs) {
      const auto count =
          multi_result::take(query.tensor_outputs->element_count());
      if (count == 1)
        ++gate->narrow_starts;
      if (count == 2)
        ++gate->wide_starts;
    }
    return ResultContinuation::make<GatedProgram>(allocator, gate, terminal,
                                                  whole, staged);
  };
  if (per_atom) {
    op.traits.outputs[0].failure_delivery = FailureDelivery::PerAtomOutcome;
    op.traits.joint_contract = 2;
    op.traits.joint_continuation_bytes = sizeof(GatedJoint);
    op.traits.joint_workspace_bytes = 512;
    op.start_result_joint = [gate](const ResourceVector<ResultProgramQuery>&,
                                   const BufferAllocator& allocator) {
      return ResultJointContinuation::make<GatedJoint>(allocator, gate);
    };
    auto consumer = copy_definition("pass", sizeof(GatedProgram));
    consumer.start_result = [](const ResultProgramQuery&,
                               const BufferAllocator& allocator) {
      return ResultContinuation::make<GatedProgram>(allocator, nullptr, false,
                                                    false, false);
    };
    multi_result::check(registry->register_operation(std::move(consumer)));
  }
  if (effects) {
    OperationDefinition effect;
    effect.key = "effect";
    effect.traits.deterministic = false;
    effect.traits.side_effect_free = false;
    effect.traits.cacheable = false;
    effect.traits.workspace_bytes = 32;
    effect.traits.outputs[0] = multi_result::output("value", tensor_schema());
    effect.traits.outputs[0].continuation_bytes = sizeof(GatedProgram);
    effect.traits.outputs[0].region_rule = OperationRegionRule::Whole;
    effect.start_result = [effects](const ResultProgramQuery&,
                                    const BufferAllocator& allocator) {
      return ResultContinuation::make<GatedProgram>(allocator, nullptr, false,
                                                    false, false, effects);
    };
    multi_result::check(registry->register_operation(std::move(effect)));
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
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto context = std::make_unique<ExecutionContext>(
      registry, ExecutionContextConfig{2, false, 8, 4096});
  ExecutionBindings bindings{
      {binding(*context, "x", values<double>(ElementType::Float64, {1, 2}))}};
  auto demand = multi_result::take(context->open_demand(plan, bindings));
  gate->open();
  PS_CHECK(demand.request({{"y", point(0, 2)}}).ok());
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->entered = 0;
    gate->release = false;
  }
  auto old = std::async(std::launch::async,
                        [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(gate->await(1));
  bindings.inputs[0].result =
      binding(*context, bindings.inputs[0].name,
              values<double>(ElementType::Float64, {3, 4}))
          .result;
  auto replaced = demand.replace_bindings(bindings);
  PS_CHECK(replaced.ok() && replaced.value().generation == 2);
  PS_CHECK(replaced.value().potential_dirty.at("y") == point(0, 2));
  gate->open();
  PS_CHECK(old.get().status().code == ErrorCode::Stale && gate->active == 0);
  auto next = demand.request({{"y", point(0, 2)}});
  PS_CHECK(next.ok() && next.value().generation == 2);
  double value = 0;
  PS_CHECK(
      numeric_result_fixture::read(next.value().results.at("y"), {0}, &value, 8)
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
  auto a = std::async(std::launch::async,
                      [&] { return demand.request({{"y", point(0, 2)}}); });
  auto b = std::async(std::launch::async, [&] {
    return demand.request({{"y", point(1, 2)}}, cancel.token());
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
  auto pending = std::async(
      std::launch::async, [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(gate->await(1));
  context.reset();
  PS_CHECK(pending.get().status().code == ErrorCode::Cancelled &&
           gate->active == 0);
  PS_CHECK(demand.request({{"y", point(0, 2)}}).status().code ==
           ErrorCode::Cancelled);
  return 0;
}
int replacement_during_shutdown() {
  auto registry = gated_registry(nullptr);
  GraphContext graph(gate_document());
  auto compiled = Compiler(registry).compile(graph);
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
  auto plan = Compiler(registry).compile(graph).take_value().plan;
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
  PS_CHECK(demand.request({{"y", point(0, 2)}}).ok());
  PS_CHECK(demand.request({{"y", point(1, 2)}}).status().code ==
           ErrorCode::OperationFailed);
  PS_CHECK(
      demand.request({{"y", Footprint::all({2}).take_value()}}).status().code ==
      ErrorCode::OperationFailed);
  PS_CHECK(demand.request({{"y", point(0, 2)}}).ok());
  auto frozen = demand.freeze().take_value();
  PS_CHECK(graph.replace(gate_document()) > 0);
  PS_CHECK(demand.request({{"y", point(0, 2)}}).ok());
  PS_CHECK(demand.cancel());
  auto fresh = context.open_demand(frozen.plan(), bindings, DemandConfig{1});
  PS_CHECK(fresh.ok());
  PS_CHECK(fresh.value().request({{"y", point(0, 2)}}).status().code ==
           ErrorCode::ResourceExhausted);
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
  auto registry = make_default_operation_registry(false);
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
    auto writer = MutableValue::allocate({type, {3}}, Region::whole({3}),
                                         BufferAllocator{})
                      .take_value();
    std::memset(writer.data(), 0, writer.size());
    auto initial = std::move(writer).publish().take_value();
    InputSnapshotStore store({256, 1});
    auto snapshot = store.import_value(initial).take_value();
    auto document = identity_document(initial);
    document.nodes[0].operation = "test.snapshot_identity";
    GraphContext graph(document);
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    ExecutionBindings bindings{
        {snapshot_binding(context, "image", snapshot, initial)}};
    auto demand = context.open_demand(plan, bindings).take_value();
    const auto q = point(0, 3).unite(point(2, 3)).take_value();
    PS_CHECK(demand.request({{"result", q}}).ok());
    auto frozen = demand.freeze().take_value();
    for (auto at : {0U, 2U}) {
      auto patch = MutableValue::allocate(initial.descriptor(),
                                          Region({{at, 1}}), BufferAllocator{})
                       .take_value();
      // Includes nonfinite generic float payloads; dirty compares bits.
      std::memset(patch.data(), 0xff, patch.size());
      snapshot = store.patch(snapshot, std::move(patch).publish().take_value())
                     .take_value();
      bindings.inputs[0] =
          snapshot_binding(context, "image", snapshot, initial);
      auto changed = demand.replace_bindings(bindings, {2, {}});
      PS_CHECK(changed.ok());
      PS_CHECK(changed.value().potential_dirty.at("result") ==
               (at == 0 ? point(0, 3) : q));
    }
    auto final = demand.request({{"result", q}});
    PS_CHECK(final.ok());
    std::uint8_t bytes[8]{};
    const auto width = Value::element_size(type);
    PS_CHECK(numeric_result_fixture::read(final.value().results.at("result"),
                                          {2}, bytes, width)
                 .ok());
    for (unsigned i = 0; i < width; ++i)
      PS_CHECK(bytes[i] == 0xff);
    auto old = context.execute_fragments(frozen, {{"result", q}});
    PS_CHECK(old.ok() &&
             numeric_result_fixture::read(old.value().results.at("result"), {2},
                                          bytes, width)
                 .ok());
    for (unsigned i = 0; i < width; ++i)
      PS_CHECK(bytes[i] == 0);
    auto same = demand.replace_bindings(bindings, {2, {}});
    PS_CHECK(same.ok() && same.value().potential_dirty.at("result").empty());
  }
  auto image =
      Value::create({ElementType::Float32, {1, 1, 4}}, Region::whole({1, 1, 4}),
                    {0, {16, 16, 4}}, std::vector<std::uint8_t>(16),
                    {encode_semantic(rgba_semantics()).take_value()});
  PS_CHECK(image.status().code == ErrorCode::TypeMismatch);
  return 0;
}
int owner_retirement() {
  auto gate = std::make_shared<Gate>();
  gate->open();
  auto registry = gated_registry(gate);
  GraphContext graph(gate_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto context = std::make_unique<ExecutionContext>(registry);
  ExecutionBindings ordinary{
      {binding(*context, "x", values<double>(ElementType::Float64, {1, 2}))}};
  auto observer = context->open_demand(plan, ordinary).take_value();
  unsigned retired = 0;
  const auto owned = [&] {
    BufferAllocator allocator([&](std::uint64_t) {
      return Result<std::shared_ptr<void>>(
          std::shared_ptr<void>(new int(0), [&, observer](void* pointer) {
            delete static_cast<int*>(pointer);
            // A legitimate reservation owner can reenter the same context.
            // All bundle retirement paths must therefore unlock first.
            const auto status = observer.generation();
            if (status.ok() || status.status().code == ErrorCode::Cancelled)
              ++retired;
          }));
    });
    auto writer = MutableValue::allocate({ElementType::Float64, {2}},
                                         Region::whole({2}), allocator)
                      .take_value();
    ExecutionBindings bindings{
        {binding(*context, "x", std::move(writer).publish().take_value())}};
    return context->open_demand(plan, std::move(bindings)).take_value();
  };
  auto cancelled = owned();
  PS_CHECK(cancelled.cancel() && retired == 1);
  auto replaced = owned();
  PS_CHECK(replaced.replace_bindings(ordinary).ok() && retired == 2);
  auto destroyed = owned();
  destroyed = DemandHandle{};
  PS_CHECK(retired == 3);
  auto closing = owned();
  context.reset();
  PS_CHECK(retired == 4 &&
           closing.generation().status().code == ErrorCode::Cancelled);
  return 0;
}
bool await_shared(const ExecutionContext& context, std::uint64_t previous) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (context.cache_statistics().shared_computations <= previous) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::yield();
  }
  return true;
}
int shared_ancestors() {
  for (unsigned scenario = 0; scenario < 6; ++scenario) {
    const auto cancelled = scenario % 3;
    auto gate = std::make_shared<Gate>();
    auto registry = gated_registry(gate, scenario >= 3);
    auto document = gate_document();
    document.nodes.push_back({2, "wait", {WorkflowNodeOutput{1, "value"}}, {}});
    document.outputs = {{"inner", 1, "value"}, {"outer", 2, "value"}};
    GraphContext graph(document);
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    ExecutionContext context(registry, {1, false, 8, 4096});
    auto demand =
        context
            .open_demand(
                plan, {{binding(context, "x",
                                values<double>(ElementType::Float64, {7, 9}))}})
            .take_value();
    CancellationSource stop_outer, stop_inner;
    auto outer = std::async(std::launch::async, [&] {
      return demand.request({{"outer", point(0, 2)}}, stop_outer.token());
    });
    PS_CHECK(gate->await(1));
    auto inner = std::async(std::launch::async, [&] {
      return demand.request({{"inner", point(0, 2)}}, stop_inner.token());
    });
    // Observe a real directory join before cancellation; no timing sleep.
    PS_CHECK(await_shared(context, 0));
    if (cancelled == 1)
      stop_outer.cancel();
    if (cancelled == 2)
      stop_inner.cancel();
    gate->open();
    auto outer_result = outer.get();
    auto inner_result = inner.get();
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
      PS_CHECK(numeric_result_fixture::read(
                   outer_result.value().results.at("outer"), {0}, &value, 8)
                   .ok() &&
               value == 7);
      if (outer_result.value().dependencies.record_count() !=
          (scenario >= 3 ? 6U : 4U))
        std::cerr << "outer scenario " << scenario << " records "
                  << outer_result.value().dependencies.record_count() << "\n";
      PS_CHECK(outer_result.value().dependencies.record_count() ==
               (scenario >= 3 ? 6U : 4U));
      PS_CHECK(outer_result.value()
                   .dependencies.potential_dirty("x", point(0, 2))
                   .value()
                   .at("outer") == point(0, 2));
    }
    if (inner_result.ok()) {
      double value = 0;
      PS_CHECK(numeric_result_fixture::read(
                   inner_result.value().results.at("inner"), {0}, &value, 8)
                   .ok() &&
               value == 7);
      PS_CHECK(inner_result.value().diagnostics.shared_computations >= 1);
      PS_CHECK(context.resource_budget()
                   .value()
                   .statistics()
                   .live[ResourceKind::Payload] >= 8);
      if (inner_result.value().dependencies.record_count() != 2U)
        std::cerr << "inner scenario " << scenario << " records "
                  << inner_result.value().dependencies.record_count() << "\n";
      PS_CHECK(inner_result.value().dependencies.record_count() == 2U);
      PS_CHECK(inner_result.value()
                   .dependencies.potential_dirty("x", point(0, 2))
                   .value()
                   .at("inner") == point(0, 2));
    }
    PS_CHECK(gate->entered == (cancelled == 1 ? 1U : 2U));
    PS_CHECK(context.cache_statistics().in_flight == 0);
  }
  return 0;
}
int auxiliary_cancellation() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate, true);
  GraphContext graph(gate_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {1, false, 8, 4096});
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
    return demand.request({{"y", point(0, 2)}}, {}, options);
  });
  PS_CHECK(gate->await(1));
  auto b = std::async(std::launch::async,
                      [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(await_shared(context, 0));
  auxiliary.cancel();
  gate->open();
  PS_CHECK(a.get().status().code == ErrorCode::Cancelled);
  auto survived = b.get();
  PS_CHECK(survived.ok() &&
           survived.value().diagnostics.shared_computations == 1);
  PS_CHECK(gate->entered == 1 && gate->active == 0);
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
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {2, false, 8, 4096});
  auto demand = context.open_demand(plan, {}).take_value();
  auto a = std::async(std::launch::async,
                      [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(gate->await(1));
  auto b = std::async(std::launch::async,
                      [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(gate->await(2));
  gate->open();
  auto first = a.get(), second = b.get();
  PS_CHECK(first.ok() && second.ok());
  double x = 0, y = 0;
  PS_CHECK(
      numeric_result_fixture::read(first.value().results.at("y"), {0}, &x, 8)
          .ok());
  PS_CHECK(
      numeric_result_fixture::read(second.value().results.at("y"), {0}, &y, 8)
          .ok());
  PS_CHECK(x == 1 && y == 2 && *effects == 2);
  PS_CHECK(context.cache_statistics().shared_computations == 0);
  PS_CHECK(first.value().dependencies.record_count() == 4 &&
           second.value().dependencies.record_count() == 4);
  return 0;
}
int shared_fallback() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate, false, {}, false, true, true);
  auto doc = gate_document();
  doc.nodes.push_back({2, "wait", {WorkflowNodeOutput{1, "value"}}, {}});
  doc.outputs = {{"y", 2, "value"}};
  GraphContext graph(doc);
  PlanningOptions planning;
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
    return demand.request({{"y", point(0, 2)}}, stop.token());
  });
  PS_CHECK(gate->await(1));
  auto other = std::async(std::launch::async,
                          [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(await_shared(context, 0));
  stop.cancel();
  gate->open();
  auto cancelled = owner.get();
  auto completed = other.get();
  double actual = 0;
  PS_CHECK(cancelled.status().code == ErrorCode::Cancelled);
  PS_CHECK(completed.ok() &&
           numeric_result_fixture::read(completed.value().results.at("y"), {0},
                                        &actual, 8)
               .ok() &&
           actual == 7);
  PS_CHECK(completed.value().diagnostics.shared_computations == 1 &&
           completed.value().diagnostics.selected_backends.at({2, 0}) ==
               Backend::Cpu);
  PS_CHECK(context.cache_statistics().retained_bytes == 0);
  PS_CHECK(
      demand.request({{"y", point(0, 2)}}).value().diagnostics.cache_hits == 0);
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
    // No Need has granted this input. The real read records Protocol first.
    auto failure = phase.read_tensor(0, 0, {0}, &value, sizeof(value));
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
  op.start_result = [probe](const ResultProgramQuery&,
                            const BufferAllocator& allocator) {
    return ResultContinuation::make<ProtocolCancelProgram>(allocator, probe);
  };
  multi_result::check(registry->register_operation(std::move(op)));
  multi_result::check(registry->freeze());
  GraphContext graph(gate_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {1, false, 8, 4096});
  auto demand =
      context
          .open_demand(
              plan, {{binding(context, "x",
                              values<double>(ElementType::Float64, {7, 9}))}})
          .take_value();
  auto failed =
      demand.request({{"y", point(0, 2)}}, probe->cancellation.token());
  PS_CHECK(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
           failed.status().message == "missing authorized image input" &&
           failed.status().reason == FailureReason::UnauthorizedRead &&
           failed.status().detail.origin == FailureOrigin::Protocol &&
           failed.status().detail.scope == FailureScope::Group &&
           !failed.status().detail.atom);
  PS_CHECK(probe->latched_before_cancel &&
           probe->cancellation.token().cancelled() && probe->starts == 1 &&
           probe->polls == 1 && probe->destroys == 1);
  PS_CHECK(context.cache_statistics().in_flight == 0 &&
           context.cache_statistics().retained_bytes == 0);
  return 0;
}
int late_flight_and_frozen() {
  auto gate = std::make_shared<Gate>();
  gate->ignore_cancellation = true;
  auto registry = gated_registry(gate, true);
  GraphContext graph(gate_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {2, false, 8, 4096});
  ExecutionBindings bindings{
      {binding(context, "x", values<double>(ElementType::Float64, {7, 9}))}};
  auto demand = context.open_demand(plan, bindings).take_value();
  CancellationSource stop;
  DemandQuery query{{"y", point(0, 2)}};
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
  PS_CHECK(await_shared(context, 0));
  gate->open();
  PS_CHECK(p1.get().ok());
  auto joined = follower.get();
  PS_CHECK(joined.ok() && joined.value().diagnostics.shared_computations == 1);
  PS_CHECK(gate->entered == 2 && context.cache_statistics().in_flight == 0);
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
  const auto shared_before = context.cache_statistics().shared_computations;
  auto pinned = std::async(std::launch::async, [&] {
    return context.execute_fragments(frozen, query);
  });
  PS_CHECK(await_shared(context, shared_before));
  bindings.inputs[0].result =
      binding(context, bindings.inputs[0].name,
              values<double>(ElementType::Float64, {8, 9}))
          .result;
  PS_CHECK(demand.replace_bindings(bindings).ok());
  gate->open();
  PS_CHECK(latest.get().status().code == ErrorCode::Stale);
  auto old = pinned.get();
  double result = 0;
  PS_CHECK(
      old.ok() &&
      numeric_result_fixture::read(old.value().results.at("y"), {0}, &result, 8)
          .ok() &&
      result == 7);
  PS_CHECK(gate->entered == 1);
  auto current = demand.request(query);
  PS_CHECK(current.ok() &&
           numeric_result_fixture::read(current.value().results.at("y"), {0},
                                        &result, 8)
               .ok() &&
           result == 8);
  return 0;
}
int shared_terminal() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate, true, {}, true);
  auto document = gate_document();
  document.inputs[0] =
      multi_result::declaration(1, "x", tensor_schema(ElementType::Float64, 3));
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {2, false, 8, 4096, 512});
  ExecutionBindings bindings{
      {binding(context, "x", values<double>(ElementType::Float64, {7, 8, 9}))}};
  auto demand = context.open_demand(plan, bindings).take_value();
  const auto wide = point(0, 3).unite(point(2, 3)).take_value();
  auto first = std::async(std::launch::async,
                          [&] { return demand.request({{"y", wide}}); });
  PS_CHECK(gate->await(1));
  auto same = std::async(std::launch::async,
                         [&] { return demand.request({{"y", wide}}); });
  PS_CHECK(await_shared(context, 0));
  auto subset = std::async(
      std::launch::async, [&] { return demand.request({{"y", point(0, 3)}}); });
  PS_CHECK(gate->await(2));
  gate->open();
  auto a = first.get(), b = same.get(), c = subset.get();
  PS_CHECK(a.ok() && b.ok() && c.ok() && gate->entered == 2);
  double value = 0;
  for (const auto* result : {&a.value(), &b.value()}) {
    PS_CHECK(
        numeric_result_fixture::read(result->results.at("y"), {2}, &value, 8)
            .ok() &&
        value == 9);
    PS_CHECK(
        !numeric_result_fixture::read(result->results.at("y"), {1}, &value, 8)
             .ok());
    PS_CHECK(!result->dependencies.restrict({{"y", point(0, 3)}}).ok());
    PS_CHECK(result->dependencies.coverage().at("y") == wide);
    PS_CHECK(result->dependencies.certificate({1, 0}).status().code ==
             ErrorCode::NotFound);
  }
  PS_CHECK(
      numeric_result_fixture::read(c.value().results.at("y"), {0}, &value, 8)
          .ok() &&
      value == 8);
  PS_CHECK(b.value().diagnostics.shared_computations == 1 &&
           c.value().diagnostics.shared_computations == 0);
  auto reused = demand.request({{"y", wide}});
  PS_CHECK(reused.ok() && reused.value().diagnostics.cache_hits == 0 &&
           reused.value().diagnostics.shared_computations > 0 &&
           reused.value().results.at("y").object_id() ==
               a.value().results.at("y").object_id());
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto warm = demand.request({{"y", wide}});
  PS_CHECK(warm.ok() && warm.value().diagnostics.cache_hits == 1 &&
           gate->entered == 2);
  PS_CHECK(!warm.value().dependencies.restrict({{"y", point(0, 3)}}).ok());
  auto changed = demand.replace_bindings({{binding(
      context, "x", values<double>(ElementType::Float64, {7, 8, 222}))}});
  PS_CHECK(changed.ok() && changed.value().potential_dirty.at("y").empty());
  auto content_hit = demand.request({{"y", wide}});
  PS_CHECK(content_hit.ok() &&
           content_hit.value().diagnostics.cache_hits == 1 &&
           gate->entered == 2);
  PS_CHECK(numeric_result_fixture::read(content_hit.value().results.at("y"),
                                        {2}, &value, 8)
               .ok() &&
           value == 9);
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
  auto plan = Compiler(registry).compile(graph).take_value().plan;
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
  auto bounded = demand.request({{"y", point(0, 2)}}, {}, options);
  PS_CHECK(bounded.ok() && gate->entered == 64);
  PS_CHECK(bounded.value().diagnostics.dependency_cache_records_visited == 0 &&
           bounded.value().diagnostics.dependency_cache_work == 0);
  PS_CHECK(context.cache_statistics().retained_bytes == 0);
  // A real outstanding producer captured the old epoch. Its successful late
  // completion cannot repopulate a cleared cache, even with a live waiter.
  GraphContext short_graph(gate_document());
  auto short_plan = Compiler(registry).compile(short_graph).take_value().plan;
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
    return short_demand.request({{"y", point(0, 2)}});
  });
  PS_CHECK(gate->await(1));
  context.clear_result_cache();
  gate->open();
  PS_CHECK(pending.get().ok() &&
           context.cache_statistics().retained_bytes == 0);
  PS_CHECK(short_demand.request({{"y", point(0, 2)}})
               .value()
               .diagnostics.cache_hits == 0);
  PS_CHECK(short_demand.replace_bindings(short_bindings).ok());
  PS_CHECK(short_demand.request({{"y", point(0, 2)}})
               .value()
               .diagnostics.cache_hits == 1);
  PS_CHECK(gate->entered == 2);
  return 0;
}
int cached_whole_ancestor() {
  auto gate = std::make_shared<Gate>();
  gate->open();
  auto registry = gated_registry(gate, false, {}, false, true);
  auto document = gate_document();
  document.nodes.push_back({2, "wait", {WorkflowNodeOutput{1, "value"}}, {}});
  document.outputs = {{"y", 2, "value"}, {"ancestor", 1, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {1, false, 8, 4096, 512});
  ExecutionBindings bindings{
      {binding(context, "x", values<double>(ElementType::Float64, {7, 9}))}};
  auto demand = context.open_demand(plan, bindings).take_value();
  auto first = demand.request({{"y", point(0, 2)}});
  PS_CHECK(first.ok() && gate->entered == 2);
  auto reused = demand.request({{"y", point(0, 2)}});
  PS_CHECK(reused.ok() && reused.value().diagnostics.cache_hits == 0 &&
           reused.value().diagnostics.shared_computations > 0 &&
           reused.value().results.at("y").object_id() ==
               first.value().results.at("y").object_id());
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto warm = demand.request({{"y", point(0, 2)}});
  PS_CHECK(warm.ok() && gate->entered == 2 &&
           warm.value().diagnostics.cache_hits == 2);
  PS_CHECK(warm.value().diagnostics.operation_timings.empty());
  PS_CHECK(warm.value().dependencies.source_support().value().at("x") ==
           Footprint::all({2}).take_value());
  double value = 0;
  PS_CHECK(
      numeric_result_fixture::read(warm.value().results.at("y"), {0}, &value, 8)
          .ok() &&
      value == 7);
  // The root's 16-byte result survives while its Whole ancestor's pixels
  // are evicted. Importing its evidence must not block a later actual read.
  document.outputs = {{"a_desc", 2, "value"}, {"z_ancestor", 1, "value"}};
  GraphContext named_graph(document);
  auto named_plan = Compiler(registry).compile(named_graph).take_value().plan;
  ExecutionContext tight(registry, {1, false, 8, 4096, 16});
  ExecutionBindings tight_bindings{
      {binding(tight, "x", values<double>(ElementType::Float64, {7, 9}))}};
  auto query = tight.open_demand(named_plan, tight_bindings).take_value();
  PS_CHECK(query.request({{"a_desc", point(0, 2)}}).ok());
  // A -> B -> C: the 16-byte LRU retains C and evicts B pixels. The
  // active C subscription must still transpose an A edit through B evidence.
  const auto old_bundle = query.freeze().take_value();
  tight_bindings.inputs[0] =
      binding(tight, "x", values<double>(ElementType::Float64, {11, 9}));
  auto change = query.replace_bindings(tight_bindings);
  PS_CHECK(change.ok() &&
           change.value().potential_dirty.at("a_desc") == point(0, 2));
  auto updated = query.request({{"a_desc", point(0, 2)}});
  PS_CHECK(updated.ok() &&
           numeric_result_fixture::read(updated.value().results.at("a_desc"),
                                        {0}, &value, 8)
               .ok() &&
           value == 11);
  auto pinned = tight.execute_fragments(old_bundle, {{"a_desc", point(0, 2)}});
  PS_CHECK(pinned.ok() &&
           numeric_result_fixture::read(pinned.value().results.at("a_desc"),
                                        {0}, &value, 8)
               .ok() &&
           value == 7);
  // Restore the current descendant to the single-entry LRU before asking for
  // both nodes; the old frozen computation must not replace current evidence.
  PS_CHECK(query.replace_bindings(tight_bindings).ok());
  PS_CHECK(query.request({{"a_desc", point(0, 2)}}).ok());
  const auto previous = gate->entered;
  PS_CHECK(query.replace_bindings(tight_bindings).ok());
  auto both =
      query.request({{"a_desc", point(0, 2)}, {"z_ancestor", point(1, 2)}});
  PS_CHECK(both.ok() && both.value().diagnostics.cache_hits == 1 &&
           gate->entered == previous + 1);
  PS_CHECK(numeric_result_fixture::read(both.value().results.at("z_ancestor"),
                                        {1}, &value, 8)
               .ok() &&
           value == 9);
  PS_CHECK(both.value().dependencies.source_support().value().at("x") ==
           Footprint::all({2}).take_value());
  // A clear invalidates even an owning candidate whose verification is active.
  // Restore C to the single-entry LRU, then block its mandatory cold B replay.
  PS_CHECK(query.replace_bindings(tight_bindings).ok());
  PS_CHECK(query.request({{"a_desc", point(0, 2)}}).ok());
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
    return query.request({{"a_desc", point(0, 2)}});
  });
  PS_CHECK(gate->await(1));
  // Only B has entered its factory: C is still verifying its cached candidate.
  PS_CHECK(gate->wide_starts == starts_before + 1);
  tight.clear_result_cache();
  PS_CHECK(tight.cache_statistics().retained_bytes == 0);
  gate->open();
  auto cleared = verifying.get();
  PS_CHECK(cleared.ok() && cleared.value().diagnostics.cache_hits == 0 &&
           gate->entered == 2 && gate->wide_starts == starts_before + 2 &&
           tight.cache_statistics().retained_bytes == 0);
  PS_CHECK(numeric_result_fixture::read(cleared.value().results.at("a_desc"),
                                        {0}, &value, 8)
               .ok() &&
           value == 11);
  PS_CHECK(numeric_result_fixture::read(cleared.value().results.at("a_desc"),
                                        {1}, &value, 8)
               .ok() &&
           value == 9);
  PS_CHECK(cleared.value().dependencies.source_support().value().at("x") ==
           Footprint::all({2}).take_value());
  return 0;
}
int cache_snapshot_bits() {
  auto registry = snapshot_registry();
  for (auto type : {ElementType::UInt8, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64}) {
    const auto width = Value::element_size(type);
    std::vector<std::uint8_t> bytes(width * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = static_cast<std::uint8_t>(255 - i);
    auto reversed =
        Value::create({type, {3}}, Region::whole({3}),
                      {width * 2, {-static_cast<std::int64_t>(width)}}, bytes)
            .take_value();
    auto writer = MutableValue::allocate({type, {3}}, Region::whole({3}),
                                         BufferAllocator{})
                      .take_value();
    for (std::size_t i = 0; i < 3; ++i)
      std::memcpy(writer.data() + i * width, bytes.data() + (2 - i) * width,
                  width);
    auto packed = std::move(writer).publish().take_value();
    auto document = identity_document(packed);
    document.nodes[0].operation = "test.snapshot_identity";
    GraphContext graph(document);
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    ExecutionContext context(registry, {1, false, 8, 4096, 512});
    auto demand =
        context.open_demand(plan, {{binding(context, "image", packed)}})
            .take_value();
    const auto q = point(0, 3).unite(point(2, 3)).take_value();
    auto initial = demand.request({{"result", q}});
    PS_CHECK(initial.ok() && initial.value().diagnostics.cache_hits == 0);
    InputSnapshotStore store({1024, 1});
    auto snapshot = std::make_shared<const InputSnapshot>(
        store.import_value(reversed).take_value());
    auto edited = demand.replace_bindings(
        {{snapshot_binding(context, "image", *snapshot, packed)}});
    PS_CHECK(edited.ok() &&
             edited.value().potential_dirty.at("result").empty());
    auto same = demand.request({{"result", q}});
    PS_CHECK(same.ok() && same.value().diagnostics.cache_hits == 1);
    for (auto at : {0U, 2U}) {
      std::uint8_t observed[8]{};
      PS_CHECK(numeric_result_fixture::read(same.value().results.at("result"),
                                            {at}, observed, width)
                   .ok());
      PS_CHECK(std::memcmp(observed, bytes.data() + (2 - at) * width, width) ==
               0);
    }
  }
  return 0;
}
std::shared_ptr<OperationRegistry> radius_cache_fixture_registry() {
  return make_default_operation_registry();
}
WorkflowDocument radius_cache_fixture_document() {
  return scatter_document();
}
int content_cache() {
  auto registry = radius_cache_fixture_registry();
  auto document = radius_cache_fixture_document();
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContextConfig config{1, false, 8, 4096};
  config.result_cache_bytes = 2048;
  ExecutionContext context(registry, config);
  ExecutionBindings bindings{
      {binding(context, "data",
               values<double>(ElementType::Float64, {1, 2, 3, 0, 5})),
       binding(context, "radius",
               values<std::int64_t>(ElementType::Int64, {0, 0, 0, 0, 0}))}};
  auto demand = context.open_demand(plan, bindings).take_value();
  const auto q = point(0).unite(point(4)).take_value();
  DemandQuery query{{"sum", q}};
  auto first = demand.request(query);
  PS_CHECK(first.ok() && first.value().diagnostics.cache_hits == 0);
  auto frozen = context.freeze(plan, bindings).take_value();
  auto reused = demand.request(query);
  PS_CHECK(reused.ok() && reused.value().diagnostics.cache_hits == 0 &&
           reused.value().diagnostics.shared_computations > 0 &&
           reused.value().results.at("sum").object_id() ==
               first.value().results.at("sum").object_id());
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto warm = demand.request(query);
  PS_CHECK(warm.ok() && warm.value().diagnostics.cache_hits == 1 &&
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
  PS_CHECK(unchanged.ok() && unchanged.value().diagnostics.cache_hits == 1);
  PS_CHECK(unchanged.value().results.at("sum").association() !=
           first.value().results.at("sum").association());
  // Cached and freshly computed rows must merge under the current identity.
  auto mixed = demand.request({{"sum", point(0).unite(point(1)).take_value()}});
  PS_CHECK(mixed.ok() && mixed.value().diagnostics.cache_hits == 0);
  bindings.inputs[1].result =
      binding(context, bindings.inputs[1].name,
              values<std::int64_t>(ElementType::Int64, {0, 0, 0, 3, 0}))
          .result;
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto changed_relation = demand.request(query);
  PS_CHECK(changed_relation.ok() &&
           changed_relation.value().diagnostics.cache_hits == 0);
  double answer = 0;
  PS_CHECK(numeric_result_fixture::read(
               changed_relation.value().results.at("sum"), {0}, &answer, 8)
               .ok() &&
           answer == 1);
  PS_CHECK(changed_relation.value()
               .dependencies.potential_dirty("data", point(3))
               .value()
               .at("sum") == q);
  PS_CHECK(demand.replace_bindings(bindings).ok());
  PS_CHECK(demand.request(query).value().diagnostics.cache_hits == 1);
  auto old = context.execute_fragments(frozen, query);
  PS_CHECK(old.ok() && old.value().diagnostics.cache_hits == 1);
  PS_CHECK(old.value()
               .dependencies.potential_dirty("data", point(3))
               .value()
               .at("sum")
               .empty());
  bindings.inputs[0].result =
      binding(context, bindings.inputs[0].name,
              values<double>(ElementType::Float64, {1, 2, 777, 9, 5}))
          .result;
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto latest = demand.request(query);
  PS_CHECK(latest.ok() && latest.value().diagnostics.cache_hits == 0);
  PS_CHECK(numeric_result_fixture::read(latest.value().results.at("sum"), {4},
                                        &answer, 8)
               .ok() &&
           answer == 14);
  context.clear_result_cache();
  PS_CHECK(context.cache_statistics().retained_bytes == 0);
  PS_CHECK(latest.value()
               .dependencies.potential_dirty("data", point(3))
               .value()
               .at("sum") == q);
  PS_CHECK(demand.request(query).value().diagnostics.cache_hits == 0);
  config.result_cache_bytes = 16;
  ExecutionContext small(registry, config);
  ExecutionBindings small_bindings{
      {binding(small, "data",
               values<double>(ElementType::Float64, {1, 2, 777, 9, 5})),
       binding(small, "radius",
               values<std::int64_t>(ElementType::Int64, {0, 0, 0, 3, 0}))}};
  auto limited = small.open_demand(plan, small_bindings).take_value();
  PS_CHECK(limited.request({{"sum", point(0)}}).ok());
  PS_CHECK(limited.request({{"sum", point(1)}}).ok());
  PS_CHECK(limited.request({{"sum", point(2)}}).ok());
  auto all = limited.request({{"sum", Footprint::all({5}).take_value()}});
  PS_CHECK(all.ok() && small.cache_statistics().evictions > 0);
  PS_CHECK(all.value()
               .dependencies.potential_dirty("radius", point(3))
               .value()
               .at("sum") == Footprint::all({5}).take_value());
  bindings.inputs[0].result =
      binding(context, bindings.inputs[0].name,
              values<double>(ElementType::Float64, {1, 2, 777, 10, 5}))
          .result;
  small_bindings.inputs[0] = binding(
      small, "data", values<double>(ElementType::Float64, {1, 2, 777, 10, 5}));
  auto edit = limited.replace_bindings(small_bindings);
  PS_CHECK(edit.ok() && edit.value().potential_dirty.at("sum") ==
                            Footprint::all({5}).take_value());
  return 0;
}
int structured_content_cache() {
  auto registry = make_default_operation_registry();
  GraphContext graph(scatter_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContextConfig config{1, false, 8, 4096};
  config.result_cache_bytes = 2048;
  ExecutionContext context(registry, config);
  const auto root = context.resource_budget().take_value();
  const auto data = [&](std::vector<double> numbers) {
    return source(root, values<double>(ElementType::Float64, numbers));
  };
  const auto radius = [&](std::vector<std::int64_t> numbers) {
    return source(root, values<std::int64_t>(ElementType::Int64, numbers));
  };
  ExecutionBindings bindings{
      {{"data", data({1, 2, 3, 0, 5})}, {"radius", radius({0, 0, 0, 0, 0})}}};
  auto demand = context.open_demand(plan, bindings).take_value();
  const auto q = point(0).unite(point(4)).take_value();
  DemandQuery query{{"sum", q}};
  std::uint64_t first_id = 0;
  const void* first_storage = nullptr;
  {
    auto first = demand.request(query);
    PS_CHECK(first.ok() && first.value().diagnostics.cache_hits == 0);
    const auto& output = first.value().results.at("sum");
    first_id = output.object_id();
    auto window = output
                      .acquire_tensor(output.descriptor().take_value(), 0,
                                      Region({{0, 1}}))
                      .take_value();
    first_storage = window.storage_owner_token();
  }
  PS_CHECK(context.cache_statistics().entries == 1 &&
           context.cache_statistics().retained_bytes > 0);
  bindings.inputs[0].result = data({1, 2, 777, 0, 5});
  bindings.inputs[1].result = radius({0, 0, 0, 0, 0});
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto warm = demand.request(query);
  if (!warm.ok())
    std::cerr << warm.status().message << '\n';
  PS_CHECK(warm.ok() && warm.value().diagnostics.cache_hits == 1 &&
           warm.value().diagnostics.operation_timings.empty());
  auto held = warm.value().results.at("sum");
  PS_CHECK(held.object_id() != first_id);
  const auto association = held.association();
  PS_CHECK(association.size() == 2 &&
           association[0] == bindings.inputs[0].result.object_id() &&
           association[1] == bindings.inputs[1].result.object_id());
  auto window =
      held.acquire_tensor(held.descriptor().take_value(), 0, Region({{0, 1}}))
          .take_value();
  PS_CHECK(window.storage_owner_token() == first_storage);
  double number = 0;
  PS_CHECK(numeric_result_fixture::read(held, {4}, &number, 8).ok() &&
           number == 5);
  PS_CHECK(warm.value()
               .dependencies.potential_dirty("data", point(2))
               .value()
               .at("sum")
               .empty());
  bindings.inputs[1].result = radius({0, 0, 0, 3, 0});
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto routed = demand.request(query);
  PS_CHECK(routed.ok() && routed.value().diagnostics.cache_hits == 0 &&
           routed.value()
                   .dependencies.potential_dirty("data", point(3))
                   .value()
                   .at("sum") == q);
  bindings.inputs[0].result = data({1, 2, 777, 9, 5});
  PS_CHECK(demand.replace_bindings(bindings).ok());
  auto changed = demand.request(query);
  PS_CHECK(changed.ok() && changed.value().diagnostics.cache_hits == 0 &&
           numeric_result_fixture::read(changed.value().results.at("sum"), {4},
                                        &number, 8)
               .ok() &&
           number == 14);
  context.clear_result_cache();
  PS_CHECK(context.cache_statistics().retained_bytes == 0 &&
           context.cache_statistics().entries == 0);
  PS_CHECK(numeric_result_fixture::read(held, {4}, &number, 8).ok() &&
           number == 5);
  bindings.inputs[0].result = data({1, 2, 777, 9, 5});
  bindings.inputs[1].result = radius({0, 0, 0, 3, 0});
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
             numeric_result_fixture::read(completed.value().results.at("sum"),
                                          {4}, &number, 8)
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
           {"radius",
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
int shared_failure_isolation() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate, true);
  GraphContext graph(gate_document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {2, false, 8, 4096});
  ExecutionBindings bindings{
      {binding(context, "x",
               values<double>(ElementType::Float64,
                              {1, std::numeric_limits<double>::infinity()}))}};
  auto demand = context.open_demand(plan, bindings).take_value();
  const DemandQuery wide{{"y", Footprint::all({2}).take_value()}};
  auto producer =
      std::async(std::launch::async, [&] { return demand.request(wide); });
  PS_CHECK(gate->await(1));
  auto follower =
      std::async(std::launch::async, [&] { return demand.request(wide); });
  PS_CHECK(await_shared(context, 0));
  auto narrow = std::async(
      std::launch::async, [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(gate->await(2));
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
           numeric_result_fixture::read(survived.value().results.at("y"), {0},
                                        &value, 8)
               .ok() &&
           value == 1);
  PS_CHECK(gate->entered == 2 && gate->narrow_starts == 1 &&
           gate->wide_starts == 1);
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
  auto later_atom = std::async(
      std::launch::async, [&] { return demand.request({{"y", point(1, 2)}}); });
  PS_CHECK(gate->await(1));
  auto ordered = std::async(std::launch::async, [&] {
    return demand.request({{"y", Footprint::all({2}).take_value()}});
  });
  PS_CHECK(gate->await(2));
  gate->allow(1);
  PS_CHECK(later_atom.get().status().message == "nonfinite sample 1");
  gate->open();
  PS_CHECK(ordered.get().status().message == "nonfinite sample 0");
  return 0;
}
int joint_failure_isolation() {
  auto gate = std::make_shared<Gate>();
  auto registry = gated_registry(gate, false, {}, false, false, false, true);
  auto document = gate_document();
  document.nodes.push_back({2, "pass", {WorkflowNodeOutput{1, "value"}}, {}});
  document.outputs = {{"y", 2, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {2, false, 8, 4096});
  ExecutionBindings bindings{
      {binding(context, "x",
               values<double>(ElementType::Float64,
                              {1, std::numeric_limits<double>::infinity()}))}};
  auto demand = context.open_demand(plan, bindings).take_value();
  auto wide = std::async(std::launch::async, [&] {
    return demand.request({{"y", Footprint::all({2}).take_value()}});
  });
  PS_CHECK(gate->await(1));
  auto narrow = std::async(
      std::launch::async, [&] { return demand.request({{"y", point(0, 2)}}); });
  PS_CHECK(await_shared(context, 0));
  gate->open();
  auto failed = wide.get(), survived = narrow.get();
  PS_CHECK(failed.status().code == ErrorCode::OperationFailed &&
           failed.status().message == "nonfinite sample 1");
  PS_CHECK(failed.status().reason == FailureReason::InvalidDomain &&
           failed.status().detail.origin == FailureOrigin::Domain &&
           failed.status().detail.scope == FailureScope::Atom &&
           failed.status().detail.atom &&
           failed.status().detail.atom->rank == 1 &&
           failed.status().detail.atom->coordinate ==
               (std::array<std::uint64_t, 8>{1}));
  PS_CHECK(gate->joint_starts > 0 && gate->joint_polls > 0);
  double value = 0;
  PS_CHECK(survived.ok() &&
           numeric_result_fixture::read(survived.value().results.at("y"), {0},
                                        &value, 8)
               .ok() &&
           value == 1);
  PS_CHECK(gate->entered == 2 &&
           context.cache_statistics().shared_computations >= 1);
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
  auto later_atom = std::async(
      std::launch::async, [&] { return demand.request({{"y", point(1, 2)}}); });
  PS_CHECK(gate->await(1));
  auto ordered = std::async(std::launch::async, [&] {
    return demand.request({{"y", Footprint::all({2}).take_value()}});
  });
  PS_CHECK(gate->await(2));
  gate->allow(1);
  PS_CHECK(later_atom.get().status().message == "nonfinite sample 1");
  gate->open();
  auto first_failure = ordered.get();
  PS_CHECK(first_failure.status().message == "nonfinite sample 0" &&
           first_failure.status().reason == FailureReason::InvalidDomain &&
           first_failure.status().detail.scope == FailureScope::Atom &&
           first_failure.status().detail.atom &&
           first_failure.status().detail.atom->rank == 1 &&
           first_failure.status().detail.atom->coordinate ==
               (std::array<std::uint64_t, 8>{0}));
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
  PS_CHECK(owner_retirement() == 0);
  PS_CHECK(shared_ancestors() == 0);
  PS_CHECK(joint_failure_isolation() == 0);
  PS_CHECK(shared_failure_isolation() == 0);
  PS_CHECK(auxiliary_cancellation() == 0);
  PS_CHECK(impure_ancestor() == 0);
  PS_CHECK(shared_fallback() == 0);
  PS_CHECK(protocol_first_cause() == 0);
  PS_CHECK(shared_terminal() == 0);
  PS_CHECK(content_cache() == 0);
  PS_CHECK(structured_content_cache() == 0);
  PS_CHECK(cache_work_and_epoch() == 0);
  PS_CHECK(cache_snapshot_bits() == 0);
  PS_CHECK(late_flight_and_frozen() == 0);
  PS_CHECK(cached_whole_ancestor() == 0);
  return 0;
}
