#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "data/memory_budget.hpp"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/result_input_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Poll = Result<ResultProgramPoll>;
Footprint point(std::uint64_t at, std::uint64_t size = 16) {
  return multi_result::take(
      Footprint::from_regions({1, size}, {Region({{0, 1}, {at, 1}})}));
}
template <class T>
Value values(ElementType type, const std::vector<T>& samples) {
  auto writer = multi_result::take(MutableValue::allocate(
      {type, {1, samples.size()}}, Region::whole({1, samples.size()}),
      BufferAllocator{}));
  std::memcpy(writer.data(), samples.data(), writer.size());
  return multi_result::take(std::move(writer).publish());
}
SchemaTemplate schema(ElementType type = ElementType::Float64,
                      std::uint64_t size = 16) {
  auto result = multi_result::schema(type, {1, size});
  result.tensors[0].layout.spatial = true;
  result.tensors[0].layout.channel_axis.reset();
  result.id = "test.result.input";
  result.tensors[0].key = "data";
  return result;
}
ExecutionBinding source(const ResourceBudget& root, const std::string& name,
                        const Value& backing) {
  auto builder = multi_result::take(
      ResultBuilder::start(root,
                           schema(backing.descriptor().element_type,
                                  backing.descriptor().shape.back()),
                           "test.source", {}, {}, 1));

  multi_result::check(builder.publish_tensor(
      0, backing.region(), backing.layout(),
      multi_result::take(root.reference(backing.storage()))));
  return {name, multi_result::take(builder.seal())};
}
ResultBuilder builder(const ResultProgramPhase& phase) {
  auto result = multi_result::take(ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {},
      phase.association ? std::vector<std::uint64_t>(phase.association->begin(),
                                                     phase.association->end())
                        : std::vector<std::uint64_t>{},
      phase.query.tile_extent));

  return result;
}
Poll number(const ResultProgramPhase& phase, double value = 7) {
  const auto shape =
      phase.query.output.result_schema->tensors[0].sample_shape();
  const auto outputs = phase.query.tensor_outputs.value_or(
      multi_result::take(Footprint::all(shape)));
  auto output = builder(phase);
  for (const auto& box : outputs.boxes()) {
    auto bytes = multi_result::take(
        phase.allocator.allocate(multi_result::take(box.element_count()) * 8));
    for (std::size_t i = 0; i < bytes.size(); i += 8)
      std::memcpy(bytes.data() + i, &value, 8);
    multi_result::check(output.publish_tensor(
        0, box,
        {0, {8, 8}, {box.dimensions()[0].offset, box.dimensions()[1].offset}},
        std::move(bytes).freeze()));
  }
  return Poll(ResultPublication{multi_result::take(output.seal()), true});
}
struct Counts {
  std::atomic<int> starts{0}, destroyed{0}, polls{0};
};
struct ProbeState {
  std::function<Poll(const ResultProgramPhase&)> callback;
  std::shared_ptr<Counts> counts;
  ProbeState(std::function<Poll(const ResultProgramPhase&)> callback,
             std::shared_ptr<Counts> counts)
      : callback(std::move(callback)), counts(std::move(counts)) {
    ++this->counts->starts;
  }
  ~ProbeState() noexcept { ++counts->destroyed; }
  Poll poll(const ResultProgramPhase& phase) {
    ++counts->polls;
    return callback(phase);
  }
};
OperationTraits staged_traits(unsigned inputs, std::uint64_t state,
                              SchemaTemplate output_schema = schema()) {
  OperationTraits traits;
  traits.input_count = inputs;
  traits.input_schema.resize(inputs);
  for (auto& port : traits.input_schema) {
    port.kind = OperationPortKind::Result;
    port.rank = 2;
    port.element_type = static_cast<std::uint32_t>(ElementType::Float64);
  }
  traits.outputs[0] = multi_result::output("value", output_schema);
  traits.outputs[0].continuation_bytes = state;
  traits.outputs[0].maximum_dependency_stages = 32;
  traits.workspace_bytes = 4096;
  return traits;
}
OperationDefinition probe_definition(
    std::shared_ptr<Counts> counts,
    std::function<Poll(const ResultProgramPhase&)> callback =
        [](const ResultProgramPhase& phase) { return number(phase); }) {
  OperationDefinition operation;
  operation.key = "probe";
  operation.traits = staged_traits(1, sizeof(ProbeState));
  operation.start_result = [counts, callback](
                               const ResultProgramQuery&,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<ProbeState>(allocator, callback, counts);
  };
  return operation;
}
WorkflowDocument probe_document(const std::string& operation = "probe") {
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "x", schema())};
  document.nodes = {{1, operation, {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"result", 1, "value"}};
  return document;
}
Result<ExecutionResult> execute_probe(OperationDefinition operation,
                                      ExecutionOptions options = {}) {
  auto registry = std::make_shared<OperationRegistry>();
  multi_result::check(registry->register_operation(std::move(operation)));
  multi_result::check(registry->freeze());
  GraphContext graph(probe_document());
  auto plan = multi_result::take(
                  Compiler(registry).compile(
                      graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1}))
                  .plan;
  plan = multi_result::take(plan.tile_plan(
      "result", TileRequest{false, multi_result::take(TileSet::from_indices(
                                       ResourceBudget{}, {0}))}));
  ExecutionContext context(registry, {1, false, 8, 65536});
  const auto root = multi_result::take(context.resource_budget());
  return context.execute(
      plan,
      {{source(root, "x",
               values<double>(ElementType::Float64, std::vector<double>(16)))}},
      {}, options);
}
struct PointerState {
  std::shared_ptr<Counts> counts;
  std::uint64_t next = 0;
  unsigned stage = 0;
  explicit PointerState(std::shared_ptr<Counts> counts)
      : counts(std::move(counts)) {
    ++this->counts->starts;
  }
  ~PointerState() noexcept { ++counts->destroyed; }
  Poll poll(const ResultProgramPhase& phase) {
    ++counts->polls;
    const auto shape =
        phase.query.output.result_schema->tensors[0].sample_shape();
    const auto outputs = phase.query.tensor_outputs.value_or(
        multi_result::take(Footprint::all(shape)));
    if (outputs.empty())
      return number(phase);
    const auto output = outputs.boxes()[0].dimensions()[1].offset;
    const auto request = [&](unsigned port, std::uint64_t at) {
      return Poll(
          ResultProgramNeed{{multi_result::take(sdk::tile_need_for_samples(
              phase.resources, phase.query, port, 0, point(at)))}});
    };
    if (stage == 0) {
      next = output;
      stage = 1;
      return request(0, next);
    }
    if (stage == 1) {
      std::int64_t pointer = 0;
      auto status = phase.read_tensor(0, 0, {0, next}, &pointer, 8);
      if (!status.ok())
        return Poll(status);
      if (pointer >= 0) {
        next = pointer;
        return request(0, next);
      }
      next = static_cast<std::uint64_t>(-(pointer + 1));
      stage = 2;
      return request(1, next);
    }
    double value = 0;
    auto status = phase.read_tensor(1, 0, {0, next}, &value, 8);
    if (!status.ok())
      return Poll(status);
    return number(phase, value);
  }
};
OperationDefinition pointer_definition(std::shared_ptr<Counts> counts) {
  OperationDefinition operation;
  operation.key = "pointer";
  operation.traits = staged_traits(2, sizeof(PointerState));
  operation.traits.input_schema[0].element_type =
      static_cast<std::uint32_t>(ElementType::Int64);
  operation.start_result = [counts](const ResultProgramQuery&,
                                    const BufferAllocator& allocator) {
    return ResultContinuation::make<PointerState>(allocator, counts);
  };
  return operation;
}
int progressive() {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  multi_result::check(registry->register_operation(pointer_definition(counts)));
  multi_result::check(registry->freeze());
  WorkflowDocument document;
  document.inputs = {
      multi_result::declaration(1, "control", schema(ElementType::Int64)),
      multi_result::declaration(2, "payload", schema())};
  document.nodes = {{1,
                     "pointer",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"result", 1, "value"}};
  GraphContext graph(document);
  auto plan = multi_result::take(
                  Compiler(registry).compile(
                      graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1}))
                  .plan;
  plan = multi_result::take(plan.tile_plan(
      "result", TileRequest{false, multi_result::take(TileSet::from_indices(
                                       ResourceBudget{}, {0}))}));
  ExecutionContext context(registry, {1, false, 8, 65536});
  const auto root = multi_result::take(context.resource_budget());
  std::vector<std::int64_t> pointers(16, -1);
  pointers[0] = 1;
  pointers[1] = 3;
  pointers[3] = -16;
  std::vector<double> samples(16);
  for (unsigned i = 0; i < 16; ++i)
    samples[i] = i * 2;
  ExecutionBindings bindings{
      {source(root, "control", values(ElementType::Int64, pointers)),
       source(root, "payload", values(ElementType::Float64, samples))}};
  auto result = context.execute(plan, bindings);
  PS_CHECK(result.ok());
  double output = 0;
  PS_CHECK(result_input_fixture::read(result.value().results.at("result"),
                                      {0, 0}, &output, 8)
               .ok() &&
           output == 30);
  PS_CHECK(counts->polls == 5 && counts->starts == 1 && counts->destroyed == 1);
  auto record =
      multi_result::take(result.value().dependencies.tile_record({0, 0, 0}));
  PS_CHECK(record && record->inputs.size() == 2);
  PS_CHECK(record->inputs[0].tiles ==
           multi_result::take(TileSet::from_indices(root, {0, 1, 3})));
  PS_CHECK(record->inputs[1].tiles ==
           multi_result::take(TileSet::from_indices(root, {15})));
  for (auto tile : {2u, 3u}) {
    ResourceVector<TileChange> changes{ResourceAllocator<TileChange>(root)};
    changes.push_back({PlanWorkflowInput{0}, 0,
                       multi_result::take(TileSet::from_indices(root, {tile})),
                       false});
    auto dirty =
        multi_result::take(result.value().dependencies.dirty_units(changes));
    PS_CHECK(dirty.size() == (tile == 3 ? 1u : 0u));
  }
  auto frozen = multi_result::take(context.freeze(plan, bindings));
  pointers[3] = 0;
  bindings.inputs[0] =
      source(root, "control", values(ElementType::Int64, pointers));
  auto cycle = context.execute(plan, bindings);
  PS_CHECK(cycle.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(counts->starts == counts->destroyed);
  PS_CHECK(context.execute(frozen).ok());
  auto demand = multi_result::take(context.open_demand(plan, bindings));
  const auto before = counts->starts.load();
  const auto polls_before = counts->polls.load();
  auto empty = demand.request({{"result", TileRequest{}}});
  PS_CHECK(
      empty.ok() && counts->starts == before && counts->polls == polls_before &&
      counts->starts == counts->destroyed &&
      !multi_result::take(empty.value().dependencies.tile_record({0, 0, 0})));
  const auto& empty_object = empty.value().results.at("result");
  PS_CHECK(empty_object.descriptor().value().tensor_coverage(0).empty());
  PS_CHECK(!result_input_fixture::read(empty_object, {0, 0}, &output, 8).ok());
  return 0;
}
ResultRef block_state(const ResultProgramPhase& phase, double value,
                      bool foreign = false, bool bad_type = false) {
  auto root = foreign ? ResourceBudget{} : phase.resources;
  auto spec = multi_result::schema(bad_type ? ElementType::Int64
                                            : ElementType::Float64);
  auto result =
      multi_result::take(ResultBuilder::start(root, spec, "test.block"));

  auto bytes = multi_result::take(root.allocator().allocate(8));
  std::memcpy(bytes.data(), &value, 8);
  multi_result::check(result.publish_tensor(0, Region::whole({1}), {0, {8}},
                                            std::move(bytes).freeze()));
  return multi_result::take(result.seal());
}
int block_services() {
  unsigned computations = 0;
  auto counts = std::make_shared<Counts>();
  auto operation =
      probe_definition(counts, [&](const ResultProgramPhase& phase) {
        auto incoming = block_state(phase, 1);
        auto result = phase.block(1, 0, 1, 1, incoming, [&] {
          ++computations;
          return Result<ResultRef>(block_state(phase, 2));
        });
        if (!result.ok())
          return number(phase);
        return number(phase, multi_result::number(result.value()));
      });
  auto registry = std::make_shared<OperationRegistry>();
  multi_result::check(registry->register_operation(operation));
  multi_result::check(registry->freeze());
  GraphContext graph(probe_document());
  auto plan = multi_result::take(
                  Compiler(registry).compile(
                      graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1}))
                  .plan;
  ExecutionContext context(registry, {1, false, 8, 65536, 64});
  auto input =
      source(multi_result::take(context.resource_budget()), "x",
             values<double>(ElementType::Float64, std::vector<double>(16)));
  for (unsigned run = 0; run < 3; ++run) {
    auto selected = multi_result::take(plan.tile_plan(
        "result", TileRequest{false, multi_result::take(TileSet::from_indices(
                                         ResourceBudget{}, {run}))}));
    auto executed = context.execute(selected, {{input}});
    PS_CHECK(executed.ok());
    double value = 0;
    PS_CHECK(result_input_fixture::read(executed.value().results.at("result"),
                                        {0, run}, &value, 8)
                 .ok() &&
             value == 2);
    PS_CHECK(computations == 1);
    if (run)
      PS_CHECK(executed.value().diagnostics.block_cache_hits == 1 &&
               executed.value().diagnostics.cache_hits == 0);
  }
  for (unsigned bad = 0; bad < 4; ++bad) {
    auto failing = probe_definition(
        std::make_shared<Counts>(), [bad](const ResultProgramPhase& phase) {
          auto incoming = block_state(phase, 1);
          auto result =
              phase.block(1, 0, 1, 1, incoming, [&]() -> Result<ResultRef> {
                if (bad == 0)
                  throw std::bad_alloc();
                if (bad == 1)
                  throw std::runtime_error("block host");
                return Result<ResultRef>(
                    block_state(phase, 2, bad == 2, bad == 3));
              });
          static_cast<void>(result);
          return number(phase);
        });
    auto failed = execute_probe(failing);
    PS_CHECK(failed.status().code == (bad == 0   ? ErrorCode::ResourceExhausted
                                      : bad == 1 ? ErrorCode::OperationFailed
                                                 : ErrorCode::InvalidArgument));
  }
  return 0;
}
int service_and_identity_regressions() {
  for (unsigned action = 0; action < 3; ++action) {
    auto counts = std::make_shared<Counts>();
    auto op =
        probe_definition(counts, [action](const ResultProgramPhase& phase) {
          if (action == 0)
            static_cast<void>(phase.allocator.allocate(4097));
          if (action == 1) {
            double value = 0;
            static_cast<void>(phase.read_tensor(0, 0, {0, 0}, &value, 8));
          }
          if (action == 2)
            static_cast<void>(phase.consume_work(UINT64_MAX));
          return number(phase);
        });
    auto result = execute_probe(op);
    PS_CHECK(result.status().code == (action == 0 || action == 2
                                          ? ErrorCode::ResourceExhausted
                                          : ErrorCode::InvalidArgument));
    PS_CHECK(counts->starts == 1 && counts->destroyed == 1);
  }
  // Start exceptions cannot escape; cancellation takes precedence over them.
  for (unsigned action = 0; action < 4; ++action) {
    for (bool stopped : {false, true}) {
      auto counts = std::make_shared<Counts>();
      CancellationSource cancel;
      OperationRegistry registry;
      auto operation = probe_definition(counts);
      operation.start_result =
          [&](const ResultProgramQuery&,
              const BufferAllocator& allocator) -> Result<ResultContinuation> {
        auto state = ResultContinuation::make<ProbeState>(
            allocator,
            [](const ResultProgramPhase& phase) { return number(phase); },
            counts);
        if (stopped)
          cancel.cancel();
        if (action == 0)
          throw std::bad_alloc();
        if (action == 1)
          throw std::runtime_error("start failed");
        if (action == 2)
          throw 1;
        return Result<ResultContinuation>(ResultContinuation{});
      };
      multi_result::check(registry.register_operation(operation));
      ResultProgramMetadata metadata;
      metadata.inputs.resize(1);
      metadata.inputs[0].result_schema =
          std::make_shared<const SchemaTemplate>(schema());
      metadata.output.result_schema = metadata.inputs[0].result_schema;
      const std::map<std::string, ParameterValue> parameters;
      ResultProgramQuery query(metadata, parameters);
      query.semantic_key = "probe";
      query.tensor_outputs = point(0);
      query.cancellation = cancel.token();
      ResourceBudget root;
      auto started = registry.start_result("probe", query, root.allocator());
      PS_CHECK(started.status().code ==
               (stopped       ? ErrorCode::Cancelled
                : action == 0 ? ErrorCode::ResourceExhausted
                : action == 3 ? ErrorCode::InvalidArgument
                              : ErrorCode::OperationFailed));
      PS_CHECK(counts->starts == counts->destroyed);
    }
  }
  auto counts = std::make_shared<Counts>();
  auto bypass = probe_definition(counts);
  bypass.start_result = [counts](const ResultProgramQuery&,
                                 const BufferAllocator&) {
    return ResultContinuation::make<ProbeState>(
        BufferAllocator{},
        [](const ResultProgramPhase& phase) { return number(phase); }, counts);
  };
  auto rejected = execute_probe(bypass);
  PS_CHECK(rejected.status().code == ErrorCode::InvalidArgument);
  PS_CHECK(counts->starts == counts->destroyed);
  return 0;
}
struct DirectHost {
  ResourceBudget root;
  ResultProgramMetadata metadata;
  std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query;
  ResultObjectInputs inputs;
  BufferAllocator allocator;
  std::shared_ptr<std::atomic<ErrorCode>> failure;
  ResultProgramPhase phase;
  DirectHost()
      : query(metadata, parameters),
        allocator(root.allocator().limited(4096)),
        failure(std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok)),
        phase{query,
              inputs,
              allocator,
              root,
              [this](std::uint64_t count) { return root.consume({count}); },
              failure} {
    metadata.inputs.resize(1);
    metadata.inputs[0].result_schema =
        std::make_shared<const SchemaTemplate>(schema());
    metadata.output.result_schema = metadata.inputs[0].result_schema;
    query.tensor_outputs = point(0);
    query.semantic_key = "probe";
    query.snapshot_identity = "bundle";
  }
};
int concurrent_and_reentrant() {
  auto counts = std::make_shared<Counts>();
  ResultContinuation* active = nullptr;
  auto definition =
      probe_definition(counts, [&](const ResultProgramPhase& phase) {
        const auto before = phase.resources.statistics();
        auto rejected = active->poll(phase);
        const auto after = phase.resources.statistics();
        if (rejected.status().code != ErrorCode::InvalidArgument ||
            rejected.status().reason != FailureReason::None ||
            rejected.status().detail.origin != FailureOrigin::Protocol ||
            rejected.status().detail.scope != FailureScope::Group ||
            phase.failure->load() != ErrorCode::Ok ||
            before.live.values != after.live.values ||
            before.issued.work != after.issued.work)
          return Poll(Status{ErrorCode::Internal, "reentry was not rejected"});
        return number(phase);
      });
  OperationRegistry registry;
  multi_result::check(registry.register_operation(definition));
  DirectHost host;
  auto continuation = multi_result::take(
      registry.start_result("probe", host.query, host.allocator));
  unsigned observations = 0;
  host.phase.failure_observer = [&](const Status&) { ++observations; };
  active = &continuation;
  auto first = continuation.poll(host.phase);
  PS_CHECK(first.ok() && observations == 0 && counts->polls == 1);
  continuation = {};
  PS_CHECK(counts->starts == counts->destroyed);
  std::promise<void> entered, release;
  auto entrance = entered.get_future();
  auto gate = release.get_future().share();
  definition = probe_definition(counts, [&](const ResultProgramPhase& phase) {
    entered.set_value();
    gate.wait();
    return number(phase);
  });
  OperationRegistry blocking;
  multi_result::check(blocking.register_operation(definition));
  DirectHost another;
  CancellationSource cancel;
  another.query.cancellation = cancel.token();
  auto running = multi_result::take(
      blocking.start_result("probe", another.query, another.allocator));
  auto future = std::async(std::launch::async,
                           [&] { return running.poll(another.phase); });
  entrance.wait();
  const auto before = another.root.statistics();
  auto rejected = running.poll(another.phase);
  const auto after = another.root.statistics();
  PS_CHECK(rejected.status().code == ErrorCode::InvalidArgument &&
           rejected.status().detail.origin == FailureOrigin::Protocol &&
           rejected.status().detail.scope == FailureScope::Group &&
           another.failure->load() == ErrorCode::Ok);
  PS_CHECK(before.live.values == after.live.values &&
           before.issued.work == after.issued.work);
  cancel.cancel();
  PS_CHECK(counts->starts == counts->destroyed + 1);
  release.set_value();
  auto completed = future.get();
  PS_CHECK(completed.status().code == ErrorCode::Cancelled);
  PS_CHECK(running.poll(another.phase).status().code == ErrorCode::Cancelled);
  running = {};
  PS_CHECK(counts->starts == counts->destroyed);
  // An accepted exception releases the guard through the complete fence.
  auto throws = probe_definition(counts, [](const ResultProgramPhase&) -> Poll {
    throw std::runtime_error("guard exception");
  });
  OperationRegistry throwing;
  multi_result::check(throwing.register_operation(throws));
  DirectHost exception_host;
  auto original = multi_result::take(throwing.start_result(
      "probe", exception_host.query, exception_host.allocator));
  auto moved = std::move(original);
  PS_CHECK(!original.valid() && moved.valid());
  PS_CHECK(original.poll(exception_host.phase).status().code ==
           ErrorCode::Stale);
  unsigned fenced_observations = 0;
  bool fenced_reentry = true;
  exception_host.phase.failure_observer = [&](const Status& first_failure) {
    ++fenced_observations;
    const auto nested = moved.poll(exception_host.phase);
    fenced_reentry =
        fenced_reentry && nested.status().code == ErrorCode::InvalidArgument &&
        nested.status().detail.origin == FailureOrigin::Protocol &&
        nested.status().detail.scope == FailureScope::Group &&
        first_failure.code == ErrorCode::OperationFailed &&
        first_failure.message == "guard exception" &&
        exception_host.failure->load() == ErrorCode::OperationFailed;
  };
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    exception_host.failure->store(ErrorCode::Ok);
    auto failure = moved.poll(exception_host.phase);
    PS_CHECK(failure.status().code == ErrorCode::OperationFailed &&
             failure.status().reason == FailureReason::HostException &&
             failure.status().message == "guard exception");
  }
  PS_CHECK(fenced_reentry && fenced_observations == 2);
  moved = {};
  PS_CHECK(counts->starts == counts->destroyed);
  return 0;
}
int allocator_lifetime() {
  auto budget = std::make_shared<data_internal::MemoryBudget>(64);
  auto reservation = multi_result::take(budget->reserve(64));
  auto allocator = reservation->allocator().limited(16);
  auto a = multi_result::take(allocator.allocate(8));
  auto b = multi_result::take(allocator.allocate(8));
  PS_CHECK(!allocator.allocate(1).ok() && budget->live() == 16);
  reservation->seal();
  PS_CHECK(budget->live() == 16);
  a = {};
  b = {};
  PS_CHECK(budget->live() == 0 && budget->available() == 64);
  auto counts = std::make_shared<Counts>();
  OperationRegistry registry;
  multi_result::check(registry.register_operation(probe_definition(counts)));
  DirectHost host;
  auto continuation = multi_result::take(
      registry.start_result("probe", host.query, host.allocator));
  PS_CHECK(host.root.statistics().live[ResourceKind::Payload] ==
           sizeof(ProbeState));
  continuation = {};
  PS_CHECK(host.root.statistics().live[ResourceKind::Payload] == 0);
  PS_CHECK(counts->starts == counts->destroyed);
  auto small = host.root.allocator().limited(1);
  PS_CHECK(registry.start_result("probe", host.query, small).status().code ==
           ErrorCode::ResourceExhausted);
  return 0;
}
int root_work_resume() {
  for (const std::uint64_t limit : {10000U, 30000U}) {
    auto counts = std::make_shared<Counts>();
    std::uint64_t issued = 0;
    auto registry = std::make_shared<OperationRegistry>();
    auto upstream =
        probe_definition(counts, [&](const ResultProgramPhase& phase) {
          auto status = phase.consume_work(6000);
          if (!status.ok())
            return Poll(status);
          issued += 6000;
          return number(phase);
        });
    upstream.key = "fuel_upstream";
    multi_result::check(registry->register_operation(upstream));
    auto parent = probe_definition(counts);
    parent.key = "fuel_parent";
    parent.start_result = [counts, &issued](const ResultProgramQuery&,
                                            const BufferAllocator& allocator) {
      auto callback = [&issued, supplied = false](
                          const ResultProgramPhase& phase) mutable {
        if (!supplied) {
          supplied = true;
          return Poll(
              ResultProgramNeed{{multi_result::take(sdk::tile_need_for_samples(
                  phase.resources, phase.query, 0, 0, point(0)))}});
        }
        auto status = phase.consume_work(6000);
        if (!status.ok())
          return Poll(status);
        issued += 6000;
        return number(phase);
      };
      return ResultContinuation::make<ProbeState>(allocator, callback, counts);
    };
    multi_result::check(registry->register_operation(parent));
    multi_result::check(registry->freeze());
    auto document = probe_document("fuel_upstream");
    document.nodes.push_back(
        {2, "fuel_parent", {WorkflowNodeOutput{1, "value"}}, {}});
    document.outputs = {{"result", 2, "value"}};
    GraphContext graph(document);
    auto plan = multi_result::take(
                    Compiler(registry).compile(
                        graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1}))
                    .plan;
    plan = multi_result::take(plan.tile_plan(
        "result", TileRequest{false, multi_result::take(TileSet::from_indices(
                                         ResourceBudget{}, {0}))}));
    ExecutionContextConfig config{1, false, 8, 65536};
    config.managed_resources = ResourceLimits{};
    config.managed_resources->maximum_work = limit;
    ExecutionContext context(registry, config);
    auto input =
        source(multi_result::take(context.resource_budget()), "x",
               values<double>(ElementType::Float64, std::vector<double>(16)));
    ExecutionOptions options;
    options.maximum_dependency_work = limit;
    auto result = context.execute(plan, {{input}}, {}, options);
    PS_CHECK(issued == (limit == 10000 ? 6000 : 12000));
    PS_CHECK(limit == 10000
                 ? result.status().code == ErrorCode::ResourceExhausted
                 : result.ok());
    PS_CHECK(counts->starts == counts->destroyed);
  }
  return 0;
}
struct SiblingState {
  bool requested = false;
  Poll poll(const ResultProgramPhase& phase) {
    if (!requested) {
      requested = true;
      return Poll(
          ResultProgramNeed{{{0, true, false, {}}, {1, true, false, {}}}});
    }
    double sum = 0;
    for (unsigned port = 0; port < 2; ++port) {
      double value = 0;
      auto status = phase.read_tensor(port, 0, {0, 0}, &value, 8);
      if (!status.ok())
        return Poll(status);
      sum += value;
    }
    return number(phase, sum);
  }
};
struct LargeSource {
  unsigned id;
  unsigned* calls;
  std::weak_ptr<const CpuStorage>* retained;
  LargeSource(unsigned id, unsigned* calls,
              std::weak_ptr<const CpuStorage>* retained)
      : id(id), calls(calls), retained(retained) {}
  Poll poll(const ResultProgramPhase& phase) {
    constexpr std::uint64_t mib = 1024 * 1024;
    ++calls[id];
    auto scratch = phase.allocator.allocate(3 * mib);
    if (!scratch.ok())
      return Poll(scratch.status());
    auto bytes = phase.allocator.allocate(mib);
    if (!bytes.ok())
      return Poll(bytes.status());
    auto output = bytes.take_value();
    const double value = id + 1;
    std::memcpy(output.data(), &value, 8);
    auto storage = std::move(output).freeze();
    retained[id] = storage;
    auto result = builder(phase);
    multi_result::check(result.publish_tensor(
        0, Region::whole({1, mib / 8}),
        {0, {static_cast<std::int64_t>(mib), 8}}, storage));
    return Poll(ResultPublication{multi_result::take(result.seal()), true});
  }
};
int sibling_admission() {
  constexpr std::uint64_t mib = 1024 * 1024;
  auto registry = std::make_shared<OperationRegistry>();
  unsigned calls[2]{};
  std::weak_ptr<const CpuStorage> retained[2];
  for (unsigned id = 0; id < 2; ++id) {
    OperationDefinition operation;
    operation.key = id ? "sibling_b" : "sibling_a";
    operation.traits = staged_traits(0, sizeof(LargeSource),
                                     schema(ElementType::Float64, mib / 8));
    operation.traits.outputs[0].region_rule = OperationRegionRule::Whole;
    operation.traits.workspace_bytes = 4 * mib;
    operation.start_result = [&, id](const ResultProgramQuery&,
                                     const BufferAllocator& allocator) {
      return ResultContinuation::make<LargeSource>(allocator, id, calls,
                                                   retained);
    };
    multi_result::check(registry->register_operation(operation));
  }
  OperationDefinition parent;
  parent.key = "siblings";
  parent.traits =
      staged_traits(2, sizeof(SiblingState), schema(ElementType::Float64, 1));
  parent.start_result = [](const ResultProgramQuery&,
                           const BufferAllocator& allocator) {
    return ResultContinuation::make<SiblingState>(allocator);
  };
  multi_result::check(registry->register_operation(parent));
  multi_result::check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {
      {1, "sibling_a", {}, {}},
      {2, "sibling_b", {}, {}},
      {3,
       "siblings",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{2, "value"}},
       {}}};
  document.outputs = {{"sum", 3, "value"}};
  GraphContext graph(document);
  auto plan = multi_result::take(
                  Compiler(registry).compile(
                      graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1}))
                  .plan;
  ExecutionContext limited(registry, {1, false, 8, 4 * mib + 65536});
  auto failed = limited.execute(plan);
  PS_CHECK(failed.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(calls[0] == 1 && calls[1] == 1 && retained[0].expired() &&
           retained[1].expired());
  PS_CHECK(limited.resource_budget()
               .value()
               .statistics()
               .live[ResourceKind::Payload] == 0);
  document.nodes.resize(1);
  document.outputs = {{"a", 1, "value"}};
  GraphContext single_graph(document);
  auto single =
      multi_result::take(Compiler(registry).compile(single_graph)).plan;
  auto recovered = limited.execute(single);
  PS_CHECK(recovered.ok() && calls[0] == 2 && calls[1] == 1);
  PS_CHECK(recovered.value().diagnostics.peak_live_bytes >= 4 * mib);
  recovered = Result<ExecutionResult>(Status{ErrorCode::Cancelled, {}});
  PS_CHECK(retained[0].expired());
  ExecutionContext sufficient(registry, {1, false, 8, 5 * mib + 65536});
  auto complete = sufficient.execute(plan);
  PS_CHECK(complete.ok() && calls[0] == 3 && calls[1] == 2);
  PS_CHECK(multi_result::number(complete.value().results.at("sum"), {0, 0}) ==
           3);
  PS_CHECK(complete.value().diagnostics.peak_live_bytes >= 5 * mib);
  PS_CHECK(retained[0].expired() && retained[1].expired());
  return 0;
}
OperationDefinition copy_definition(std::string key) {
  OperationDefinition copy;
  copy.key = std::move(key);
  copy.traits = staged_traits(1, sizeof(multi_result::Program));
  copy.traits.input_schema[0].element_type = 0;
  copy.traits.requires_metadata_specialization = true;
  copy.specialize_metadata = [](const std::vector<OperationMetadata>& inputs,
                                const std::map<std::string, ParameterValue>&) {
    OperationOutputSpecialization output;
    output.metadata = inputs[0];
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  copy.start_result = [](const ResultProgramQuery&,
                         const BufferAllocator& allocator) {
    return ResultContinuation::make<multi_result::Program>(allocator, 0);
  };
  return copy;
}
int execution_network() {
  auto registry = std::make_shared<OperationRegistry>();
  auto counts = std::make_shared<Counts>();
  multi_result::check(registry->register_operation(pointer_definition(counts)));
  multi_result::check(registry->register_operation(copy_definition("pass")));
  std::atomic<unsigned> effects{0};
  auto effect = probe_definition(std::make_shared<Counts>(),
                                 [&](const ResultProgramPhase& phase) {
                                   ++effects;
                                   return number(phase, 1);
                                 });
  effect.key = "effect";
  effect.traits.input_count = 0;
  effect.traits.input_schema.clear();
  effect.traits.side_effect_free = false;
  effect.traits.cacheable = false;
  effect.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  multi_result::check(registry->register_operation(effect));
  multi_result::check(registry->freeze());
  WorkflowDocument document;
  document.inputs = {
      multi_result::declaration(1, "control", schema(ElementType::Int64)),
      multi_result::declaration(2, "payload", schema())};
  document.nodes = {
      {1, "pass", {WorkflowInputReference{1}}, {}},
      {2, "pass", {WorkflowInputReference{2}}, {}},
      {3,
       "pointer",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{2, "value"}},
       {}},
      {4, "pass", {WorkflowNodeOutput{3, "value"}}, {}},
      {99, "effect", {}, {}}};
  document.outputs = {{"result", 4, "value"}};
  GraphContext graph(document);
  auto plan = multi_result::take(
                  Compiler(registry).compile(
                      graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1}))
                  .plan;
  plan = multi_result::take(plan.tile_plan(
      "result", TileRequest{false, multi_result::take(TileSet::from_indices(
                                       ResourceBudget{}, {0}))}));
  ExecutionContext execution(registry, {1, false, 8, 65536});
  auto root = multi_result::take(execution.resource_budget());
  std::vector<std::int64_t> pointers(16, -1);
  pointers[0] = 1;
  pointers[1] = 3;
  pointers[3] = -16;
  std::vector<double> samples(16);
  for (unsigned i = 0; i < 16; ++i)
    samples[i] = i * 2;
  ExecutionBindings bindings{
      {source(root, "control", values(ElementType::Int64, pointers)),
       source(root, "payload", values(ElementType::Float64, samples))}};
  auto executed = execution.execute(plan, bindings);
  PS_CHECK(executed.ok() && effects == 1);
  double value = 0;
  PS_CHECK(result_input_fixture::read(executed.value().results.at("result"),
                                      {0, 0}, &value, 8)
               .ok() &&
           value == 30);
  auto record =
      multi_result::take(executed.value().dependencies.tile_record({2, 0, 0}));
  PS_CHECK(record && record->inputs.size() == 2);
  PS_CHECK(record->inputs[0].tiles ==
           multi_result::take(TileSet::from_indices(root, {0, 1, 3})));
  PS_CHECK(record->inputs[1].tiles ==
           multi_result::take(TileSet::from_indices(root, {15})));
  unsigned deliveries = 0;
  ResultTensorReadWindow retained;
  ExecutionOptions publication;
  publication.result_publication = [&](ValueRef output,
                                       const ResultRef& result) {
    if (output == ValueRef{4, 0}) {
      ++deliveries;
      retained = multi_result::take(
          result.acquire_tensor(multi_result::take(result.descriptor(false)), 0,
                                Region({{0, 1}, {0, 1}})));
    }
    return Status::success();
  };
  PS_CHECK(execution.execute(plan, bindings, {}, publication).ok());
  PS_CHECK(deliveries == 1 && effects == 2 && retained.row_run({0, 0}).ok());
  ExecutionOptions bounded;
  bounded.maximum_dependency_work = 1;
  const auto before = counts->starts.load();
  PS_CHECK(execution.execute(plan, bindings, {}, bounded).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(before == counts->starts);
  CancellationSource cancel;
  cancel.cancel();
  PS_CHECK(execution.execute(plan, bindings, cancel.token()).status().code ==
           ErrorCode::Cancelled);
  auto frozen = multi_result::take(execution.freeze(plan, bindings));
  PS_CHECK(graph.replace(document) > 0);
  PS_CHECK(execution.execute(plan, bindings).status().code == ErrorCode::Stale);
  PS_CHECK(execution.execute(frozen).ok());
  CancellationSource auxiliary;
  auxiliary.cancel();
  ExecutionOptions stopped;
  stopped.dependencies.sets.cancellation = auxiliary.token();
  const auto prior = counts->starts.load();
  PS_CHECK(execution.execute(frozen, {}, stopped).status().code ==
           ErrorCode::Cancelled);
  PS_CHECK(counts->starts == prior);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(allocator_lifetime() == 0);
  PS_CHECK(service_and_identity_regressions() == 0);
  PS_CHECK(concurrent_and_reentrant() == 0);
  PS_CHECK(root_work_resume() == 0);
  PS_CHECK(sibling_admission() == 0);
  PS_CHECK(execution_network() == 0);
  PS_CHECK(progressive() == 0);
  PS_CHECK(block_services() == 0);
  return 0;
}
