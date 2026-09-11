#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct Counts {
  std::array<std::atomic<unsigned>, 2> singles{};
  unsigned joint_starts = 0, joint_polls = 0, destroyed = 0;
  std::function<void()> hook;
};
struct Single {
  bool requested = false;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    const auto port = phase.query.output_index;
    if (!requested) {
      requested = true;
      return Result<DependencyPoll>(DependencyNeedBatch{
          {{{0},
            {{port,
              1,
              Footprint::all(phase.query.inputs[port].descriptor.shape)
                  .take_value(),
              {}}}}},
          {}});
    }
    double number = 0;
    auto status = phase.read(port, {0}, &number, sizeof(number));
    if (!status.ok())
      return Result<DependencyPoll>(status);
    MutableBuffer scratch;
    if (phase.query.inputs[port].descriptor.shape[0] > 1) {
      auto allocated = phase.allocator.allocate(
          phase.query.inputs[port].descriptor.shape[0] * 8);
      if (!allocated.ok())
        return Result<DependencyPoll>(allocated.status());
      scratch = allocated.take_value();
    }
    auto allocation =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator);
    if (!allocation.ok())
      return Result<DependencyPoll>(allocation.status());
    auto output = allocation.take_value();
    std::memcpy(output.data(), &number, sizeof(number));
    auto value = std::move(output).publish();
    if (!value.ok())
      return Result<DependencyPoll>(value.status());
    auto fragments =
        ValueFragments::create(phase.query.output.descriptor, {},
                               phase.query.outputs, {value.take_value()});
    if (!fragments.ok())
      return Result<DependencyPoll>(fragments.status());
    return Result<DependencyPoll>(fragments.take_value());
  }
};
struct Joint {
  std::shared_ptr<Counts> counts;
  std::array<Single, 2> states;
  int failure;
  Joint(std::shared_ptr<Counts> counts, int failure)
      : counts(std::move(counts)), failure(failure) {
    ++this->counts->joint_starts;
  }
  ~Joint() noexcept { ++counts->destroyed; }
  Result<std::vector<DependencyAtomOutcome>> poll(
      const DependencyJointPhase& phase) {
    ++counts->joint_polls;
    if (counts->hook)
      counts->hook();
    if (failure == 4) {
      auto allocation = MutableValue::allocate(
          {ElementType::Float64, {2}}, Region::whole({2}), phase.allocator);
      if (!allocation.ok())
        return Result<std::vector<DependencyAtomOutcome>>(allocation.status());
      auto output = allocation.take_value();
      const double numbers[2] = {7, 11};
      std::memcpy(output.data(), numbers, sizeof(numbers));
      auto backing = std::move(output).publish().take_value();
      std::vector<DependencyAtomOutcome> results;
      for (const auto* member : phase.members) {
        const auto id = member->query.output_index;
        auto view = Value::from_storage(member->query.output.descriptor,
                                        Region::whole({1}), {id * 8, {8}},
                                        backing.storage());
        if (!view.ok())
          return Result<std::vector<DependencyAtomOutcome>>(view.status());
        auto fragments =
            ValueFragments::create(member->query.output.descriptor, {},
                                   member->query.outputs, {view.take_value()});
        if (!fragments.ok())
          return Result<std::vector<DependencyAtomOutcome>>(fragments.status());
        results.push_back({id, Result<DependencyPoll>(fragments.take_value())});
      }
      return Result<std::vector<DependencyAtomOutcome>>(std::move(results));
    }
    if (failure == 1)
      throw std::runtime_error("unattributed failure");
    if (failure == 2)
      return Result<std::vector<DependencyAtomOutcome>>(
          std::vector<DependencyAtomOutcome>{});
    std::vector<DependencyAtomOutcome> result;
    for (const auto* member : phase.members) {
      const auto id = member->query.output_index;
      if (failure == 3 && id == 0)
        result.push_back(
            {id, Result<DependencyPoll>(Status{ErrorCode::OperationFailed,
                                               "local numerical error"})});
      else
        result.push_back({id, states[id].poll(*member)});
    }
    return Result<std::vector<DependencyAtomOutcome>>(std::move(result));
  }
};
OperationDefinition definition(std::shared_ptr<Counts> counts, int failure,
                               bool large) {
  OperationDefinition op;
  op.key = "test.joint_select";
  op.traits.input_count = 2;
  op.traits.input_schema.resize(2);
  op.traits.outputs.resize(2);
  op.traits.joint_contract = 1;
  op.traits.joint_continuation_bytes = large ? 8192 : sizeof(Joint);
  for (unsigned i = 0; i < 2; ++i) {
    auto& output = op.traits.outputs[i];
    output.key = i ? "right" : "left";
    output.input_indices = std::vector<std::uint32_t>{i};
    output.region_rule = OperationRegionRule::Dependency;
    output.dependency_version = 1;
    output.continuation_bytes = sizeof(Single);
    output.maximum_dependency_stages = 3;
    output.failure_delivery = FailureDelivery::PerAtomOutcome;
  }
  op.start_dependency = [counts](const DependencyQuery& query,
                                 const BufferAllocator& allocator) {
    ++counts->singles[query.output_index];
    return DependencyContinuation::make<Single>(allocator);
  };
  op.start_joint = [counts, failure](const auto&,
                                     const BufferAllocator& allocator) {
    return DependencyJointContinuation::make<Joint>(allocator, counts, failure);
  };
  return op;
}
WorkflowDocument document() {
  WorkflowDocument doc;
  doc.inputs = {
      {1, "a", {ElementType::Float64, {1}}, Region::whole({1}), {0, {8}}, {}},
      {2, "b", {ElementType::Float64, {1}}, Region::whole({1}), {0, {8}}, {}}};
  doc.nodes = {{1,
                "test.joint_select",
                {WorkflowInputReference{1}, WorkflowInputReference{2}},
                {}}};
  doc.outputs = {{"left", 1, "left"}, {"right", 1, "right"}};
  return doc;
}
double number(const DemandResult& result, const std::string& key) {
  double value = 0;
  if (!result.values.at(key).read({0}, &value, sizeof(value)).ok())
    throw std::runtime_error("sample");
  return value;
}
int projected_request_record() {
  for (bool joint : {false, true}) {
    auto counts = std::make_shared<Counts>();
    auto registry = std::make_shared<OperationRegistry>();
    auto op = definition(counts, 0, false);
    op.traits.input_count = 3;
    op.traits.input_schema.resize(3);
    auto bad = op.traits.outputs[0];
    bad.key = "bad";
    bad.input_indices = std::vector<std::uint32_t>{2};
    op.traits.outputs.push_back(bad);
    PS_CHECK(registry->register_operation(op).ok());
    auto terminal = definition(std::make_shared<Counts>(), 0, false);
    terminal.key = "test.record";
    terminal.traits.input_count = 1;
    terminal.traits.input_schema.resize(1);
    terminal.traits.outputs.resize(1);
    terminal.traits.outputs[0].key = "value";
    terminal.traits.outputs[0].observation_kind =
        ObservationKind::RequestRecord;
    terminal.traits.outputs[0].failure_delivery =
        FailureDelivery::RequestFailureOnly;
    terminal.traits.joint_contract = 0;
    terminal.traits.joint_continuation_bytes = 0;
    terminal.start_joint = {};
    auto record_calls = std::make_shared<std::atomic<unsigned>>(0);
    terminal.start_dependency = [record_calls](const DependencyQuery&,
                                               const BufferAllocator&) {
      ++*record_calls;
      return Result<DependencyContinuation>(Status{
          ErrorCode::OperationFailed, "excluded RequestRecord executed"});
    };
    PS_CHECK(registry->register_operation(terminal).ok());
    PS_CHECK(registry->freeze().ok());
    auto doc = document();
    doc.nodes[0].inputs.push_back(WorkflowNodeOutput{2, "value"});
    doc.nodes.push_back({2, terminal.key, {WorkflowInputReference{1}}, {}});
    // A downstream consumer must inherit only the selected safe result's
    // relevant ancestry, even though its sibling has a RequestRecord input.
    doc.nodes.push_back(
        {3,
         op.key,
         {WorkflowNodeOutput{1, "left"}, WorkflowNodeOutput{1, "right"},
          WorkflowNodeOutput{2, "value"}},
         {}});
    doc.outputs = {{"left", 3, "left"}, {"right", 3, "right"}};
    GraphContext graph(doc);
    auto compiled = Compiler(registry).compile(graph);
    if (!compiled.ok())
      std::cerr << compiled.status().message << '\n';
    PS_CHECK(compiled.ok());
    for (const auto& node : compiled.value().semantic.nodes())
      if (node.id != 2) {
        PS_CHECK(node.outputs[0].effective_atomic);
        PS_CHECK(node.outputs[1].effective_atomic);
        PS_CHECK(!node.outputs[2].effective_atomic);
      }
    ExecutionContext execution(registry, {1, false, 32, 4096, 2048});
    auto frozen =
        execution
            .freeze(compiled.value().plan, {{{"a", Value::from_float64(7)},
                                             {"b", Value::from_float64(11)}}})
            .take_value();
    ExecutionOptions options;
    options.enable_joint = joint;
    auto result = execution.execute_fragments(
        frozen,
        {{"left", Footprint::all({1}).take_value()},
         {"right", Footprint::all({1}).take_value()}},
        {}, options);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok() && record_calls->load() == 0);
    PS_CHECK(number(result.value(), "left") == 7 &&
             number(result.value(), "right") == 11);
    PS_CHECK((result.value().diagnostics.joint_groups > 0) == joint);
    PS_CHECK(!result.value().dependencies.certificate({2, 0}).ok());
    for (bool both : {false, true}) {
      doc.outputs = {{"bad", 1, "bad"}};
      if (both)
        doc.outputs.push_back({"left", 3, "left"});
      GraphContext forbidden(doc);
      PS_CHECK(Compiler(registry).compile(forbidden).status().code ==
               ErrorCode::InvalidArgument);
    }
  }
  return 0;
}
int scenarios() {
  for (int scenario = 0; scenario < 7; ++scenario) {
    const bool disabled = scenario == 1, large = scenario == 2;
    const int failure = scenario >= 3 && scenario <= 5 ? scenario - 2 : 0;
    const bool limited_flight = scenario == 6;
    auto counts = std::make_shared<Counts>();
    auto registry = std::make_shared<OperationRegistry>();
    PS_CHECK(
        registry->register_operation(definition(counts, failure, large)).ok());
    PS_CHECK(registry->freeze().ok());
    GraphContext graph(document());
    auto compiled = Compiler(registry).compile(graph);
    PS_CHECK(compiled.ok() &&
             compiled.value().plan.execution_groups().size() == 1);
    ExecutionContextConfig config{2, false, 32, 4096, 1024};
    if (limited_flight)
      config.maximum_dependency_flights = 1;
    ExecutionContext execution(registry, config);
    ExecutionBindings bindings{
        {{"a", Value::from_float64(7)}, {"b", Value::from_float64(11)}}};
    auto frozen = execution.freeze(compiled.value().plan, bindings);
    PS_CHECK(frozen.ok());
    ExecutionOptions options;
    options.enable_joint = !disabled;
    DemandQuery query{{"left", Footprint::all({1}).take_value()},
                      {"right", Footprint::all({1}).take_value()}};
    auto result =
        execution.execute_fragments(frozen.value(), query, {}, options);
    if (failure >= 2) {
      PS_CHECK(!result.ok());
      PS_CHECK(counts->singles[0] == 0 && counts->singles[1] == 0);
      if (failure == 3) {
        auto right = execution.execute_fragments(
            frozen.value(), {{"right", query.at("right")}});
        if (!right.ok())
          std::cerr << right.status().message << '\n';
        PS_CHECK(right.ok() && number(right.value(), "right") == 11);
        PS_CHECK(counts->singles[1] == 0 &&
                 right.value().diagnostics.cache_hits == 1);
      }
      continue;
    }
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok());
    PS_CHECK(number(result.value(), "left") == 7 &&
             number(result.value(), "right") == 11);
    if (!disabled && !large && !failure && !limited_flight) {
      PS_CHECK(counts->joint_starts == 1 && counts->joint_polls == 2);
      PS_CHECK(counts->singles[0] == 0 && counts->singles[1] == 0);
      PS_CHECK(result.value().diagnostics.joint_groups == 1);
    } else {
      PS_CHECK(counts->singles[0] == 1 && counts->singles[1] == 1);
    }
    PS_CHECK(counts->joint_starts == counts->destroyed);
    auto dirty =
        result.value().dependencies.potential_dirty("a", query.at("left"));
    PS_CHECK(dirty.ok() && dirty.value().at("left") == query.at("left") &&
             dirty.value().at("right").empty());
    auto again =
        execution.execute_fragments(frozen.value(), query, {}, options);
    PS_CHECK(again.ok() && again.value().diagnostics.cache_hits == 2);
  }
  return 0;
}
int independent_request_and_partial_hit() {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->register_operation(definition(counts, 0, false)).ok());
  PS_CHECK(registry->freeze().ok());
  GraphContext graph(document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext execution(registry, {2, false, 32, 4096, 1024});
  auto frozen = execution
                    .freeze(plan, {{{"a", Value::from_float64(7)},
                                    {"b", Value::from_float64(11)}}})
                    .take_value();
  auto left = execution.execute_fragments(
      frozen, {{"left", Footprint::all({1}).take_value()}});
  PS_CHECK(left.ok() && counts->singles[0] == 1 && counts->singles[1] == 0 &&
           counts->joint_starts == 0);
  auto both = execution.execute_fragments(
      frozen, {{"left", Footprint::all({1}).take_value()},
               {"right", Footprint::all({1}).take_value()}});
  PS_CHECK(both.ok() && both.value().diagnostics.cache_hits == 1);
  PS_CHECK(counts->singles[0] == 1 && counts->singles[1] == 1 &&
           counts->joint_starts == 0);
  return 0;
}
int consumer_and_shared_owner() {
  for (bool shared : {false, true}) {
    auto counts = std::make_shared<Counts>();
    auto registry = std::make_shared<OperationRegistry>();
    auto op = definition(counts, shared ? 4 : 0, false);
    if (shared)
      op.traits.joint_workspace_bytes = 16;
    PS_CHECK(registry->register_operation(op).ok());
    OperationDefinition add;
    add.key = "test.sum";
    add.traits.input_count = 2;
    add.traits.input_schema.resize(2);
    add.callback = [](const OperationInvocation& invocation) {
      return Result<Value>(
          Value::from_float64(invocation.inputs[0].as_float64().value() +
                              invocation.inputs[1].as_float64().value()));
    };
    PS_CHECK(registry->register_operation(add).ok());
    PS_CHECK(registry->freeze().ok());
    auto doc = document();
    if (!shared) {
      doc.nodes.push_back(
          {2,
           "test.sum",
           {WorkflowNodeOutput{1, "left"}, WorkflowNodeOutput{1, "right"}},
           {}});
      doc.outputs = {{"sum", 2, "value"}};
    }
    GraphContext graph(doc);
    auto compiled = Compiler(registry).compile(graph);
    PS_CHECK(compiled.ok());
    ExecutionContext execution(registry, {2, false, 32, 4096, 1024});
    auto frozen =
        execution
            .freeze(compiled.value().plan, {{{"a", Value::from_float64(7)},
                                             {"b", Value::from_float64(11)}}})
            .take_value();
    DemandQuery query;
    for (const auto& output : doc.outputs)
      query.emplace(output.name, Footprint::all({1}).take_value());
    auto result = execution.execute_fragments(frozen, query);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok() && counts->joint_starts == 1);
    PS_CHECK(counts->singles[0] == 0 && counts->singles[1] == 0);
    if (shared) {
      PS_CHECK(result.value().values.at("left").fragments()[0].storage() ==
               result.value().values.at("right").fragments()[0].storage());
      PS_CHECK(execution.cache_statistics().retained_bytes == 16);
    } else {
      PS_CHECK(number(result.value(), "sum") == 18);
    }
  }
  return 0;
}
int cancellation_with_external_waiter() {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->register_operation(definition(counts, 0, false)).ok());
  PS_CHECK(registry->freeze().ok());
  GraphContext graph(document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext execution(registry, {2, false, 32, 4096, 1024});
  auto frozen = execution
                    .freeze(plan, {{{"a", Value::from_float64(7)},
                                    {"b", Value::from_float64(11)}}})
                    .take_value();
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, release = false;
  counts->hook = [&] {
    if (counts->joint_polls != 2)
      return;
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    changed.notify_all();
    if (!changed.wait_for(lock, std::chrono::seconds(5),
                          [&] { return release; }))
      throw std::runtime_error("test gate timeout");
  };
  CancellationSource cancelled;
  auto first = std::async(std::launch::async, [&] {
    return execution.execute_fragments(
        frozen,
        {{"left", Footprint::all({1}).take_value()},
         {"right", Footprint::all({1}).take_value()}},
        cancelled.token());
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    PS_CHECK(changed.wait_for(lock, std::chrono::seconds(5),
                              [&] { return entered; }));
  }
  auto second = std::async(std::launch::async, [&] {
    return execution.execute_fragments(
        frozen, {{"right", Footprint::all({1}).take_value()}});
  });
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!execution.cache_statistics().shared_computations &&
         std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const bool subscribed = execution.cache_statistics().shared_computations != 0;
  cancelled.cancel();
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  changed.notify_all();
  auto a = first.get();
  auto b = second.get();
  counts->hook = {};
  if (!b.ok())
    std::cerr << b.status().message << '\n';
  PS_CHECK(subscribed && !a.ok() && a.status().code == ErrorCode::Cancelled);
  PS_CHECK(b.ok() && number(b.value(), "right") == 11);
  PS_CHECK(counts->singles[0] == 0 && counts->singles[1] == 0 &&
           counts->destroyed == 1);
  return 0;
}
int proportional_workspace_and_depth() {
  for (bool deep : {false, true}) {
    auto counts = std::make_shared<Counts>();
    auto registry = std::make_shared<OperationRegistry>();
    auto op = definition(counts, 0, false);
    op.traits.workspace_input_multiplier = 1;
    PS_CHECK(registry->register_operation(op).ok());
    PS_CHECK(registry->freeze().ok());
    auto doc = document();
    ExecutionBindings bindings;
    if (deep) {
      for (std::uint64_t id = 2; id <= 64; ++id)
        doc.nodes.push_back({id,
                             op.key,
                             {WorkflowNodeOutput{id - 1, "left"},
                              WorkflowNodeOutput{id - 1, "right"}},
                             {}});
      doc.outputs = {{"left", 64, "left"}, {"right", 64, "right"}};
      bindings.inputs = {{"a", Value::from_float64(7)},
                         {"b", Value::from_float64(11)}};
    } else {
      for (unsigned i = 0; i < 2; ++i) {
        doc.inputs[i].descriptor.shape = {128};
        doc.inputs[i].region = Region::whole({128});
        std::vector<std::uint8_t> bytes(1024);
        const double number = i ? 11 : 7;
        std::memcpy(bytes.data(), &number, sizeof(number));
        auto value =
            Value::create({ElementType::Float64, {128}}, Region::whole({128}),
                          {0, {8}}, std::move(bytes));
        PS_CHECK(value.ok());
        bindings.inputs.push_back({i ? "b" : "a", value.take_value()});
      }
    }
    GraphContext graph(doc);
    auto compiled = Compiler(registry).compile(graph);
    PS_CHECK(compiled.ok());
    ExecutionContext execution(registry, {2, false, 32, 1048576, 65536});
    auto frozen =
        execution.freeze(compiled.value().plan, bindings).take_value();
    auto result = execution.execute_fragments(
        frozen, {{"left", Footprint::all({1}).take_value()},
                 {"right", Footprint::all({1}).take_value()}});
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok());
    PS_CHECK(number(result.value(), "left") == 7 &&
             number(result.value(), "right") == 11);
    if (deep)
      PS_CHECK(counts->joint_starts == 16 && counts->singles[0] > 0 &&
               counts->singles[1] > 0);
    else
      PS_CHECK(counts->joint_starts == 1 && counts->singles[0] == 0 &&
               counts->singles[1] == 0);
  }
  return 0;
}
int fallback_ancestry() {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  auto op = definition(counts, 0, false);
  PS_CHECK(registry->register_operation(op).ok());
  OperationDefinition source;
  source.key = "test.optional_gpu";
  source.traits.supports_gpu = true;
  source.traits.allows_cpu_fallback = true;
  source.callback = [](const OperationInvocation& invocation) {
    if (invocation.backend == Backend::Gpu)
      return Result<Value>(
          Status{ErrorCode::BackendUnavailable, "test fallback"});
    return Result<Value>(Value::from_float64(7));
  };
  PS_CHECK(registry->register_operation(source).ok());
  PS_CHECK(registry->freeze().ok());
  auto doc = document();
  doc.nodes = {{1, source.key, {}, {}},
               {2,
                op.key,
                {WorkflowNodeOutput{1, "value"}, WorkflowInputReference{2}},
                {}}};
  doc.outputs = {{"left", 2, "left"}, {"right", 2, "right"}};
  GraphContext graph(doc);
  PlanningOptions options;
  options.execution_mode = ExecutionMode::MetalFp32;
  auto compiled = Compiler(registry).compile(graph, options);
  PS_CHECK(compiled.ok());
  ExecutionContext execution(registry, {2, false, 32, 4096, 1024});
  auto frozen =
      execution
          .freeze(compiled.value().plan, {{{"a", Value::from_float64(7)},
                                           {"b", Value::from_float64(11)}}})
          .take_value();
  DemandQuery query{{"left", Footprint::all({1}).take_value()},
                    {"right", Footprint::all({1}).take_value()}};
  auto first = execution.execute_fragments(frozen, query);
  if (!first.ok())
    std::cerr << first.status().message << '\n';
  PS_CHECK(first.ok() && counts->joint_starts == 1);
  auto second = execution.execute_fragments(frozen, query);
  PS_CHECK(second.ok() && second.value().diagnostics.cache_hits == 1);
  PS_CHECK(counts->singles[0] == 1 && counts->singles[1] == 0);
  return 0;
}
int c_joint_roi() {
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->load_plugin(PS_DEPENDENCY_JOINT_FIXTURE).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.nodes = {{1, "test.c_joint", {}, {{"mode", std::int64_t{0}}}}};
  document.outputs = {{"first", 1, "first"}, {"second", 1, "second"}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  PS_CHECK(compiled.ok());
  ExecutionContext execution(registry);
  auto frozen = execution.freeze(compiled.value().plan, {}).take_value();
  auto result = execution.execute_fragments(
      frozen,
      {{"first", Footprint::all({1}).take_value()},
       {"second",
        Footprint::from_regions({2}, {Region({{1, 1}})}).take_value()}});
  if (!result.ok())
    std::cerr << result.status().message << '\n';
  PS_CHECK(result.ok() && result.value().diagnostics.joint_groups == 1);
  double number = 0;
  PS_CHECK(result.value()
               .values.at("second")
               .read({1}, &number, sizeof(number))
               .ok());
  PS_CHECK(number == 11);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(projected_request_record() == 0);
  PS_CHECK(c_joint_roi() == 0);
  PS_CHECK(proportional_workspace_and_depth() == 0);
  PS_CHECK(fallback_ancestry() == 0);
  PS_CHECK(consumer_and_shared_owner() == 0);
  PS_CHECK(cancellation_with_external_waiter() == 0);
  PS_CHECK(scenarios() == 0);
  PS_CHECK(independent_request_and_partial_hit() == 0);
  return 0;
}
