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
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct Counts {
  std::array<std::atomic<unsigned>, 2> singles{};
  unsigned joint_starts = 0, joint_polls = 0, destroyed = 0;
  std::function<void()> hook;
};
using multi_result::check;
using multi_result::take;
ResultProgramPoll publish(const ResultProgramPhase& phase, double number,
                          std::shared_ptr<const CpuStorage> backing = {},
                          std::uint64_t offset = 0) {
  auto builder = take(ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {},
      phase.association ? std::vector<std::uint64_t>(phase.association->begin(),
                                                     phase.association->end())
                        : std::vector<std::uint64_t>{}));
  check(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(phase.resources, 1, {}))));
  auto relation = take(ResultRelation::cartesian(
      phase.resources, 1,
      backing ? ResultSupport{}
              : ResultSupport{phase.query.output_index, 1, 0,
                              phase.query.inputs[phase.query.output_index]
                                  .result_schema->tensors[0]
                                  .sample_count()
                                  .value(),
                              ResultSupportTarget::Tensor, 0}));
  if (backing)
    check(builder.publish_tensor(0, Region::whole({1}), {offset, {8}},
                                 std::move(backing), std::move(relation),
                                 {true, true, true, true}));
  else
    check(builder.publish_tensor(
        0, Region::whole({1}),
        {reinterpret_cast<const std::uint8_t*>(&number), sizeof(number)},
        std::move(relation), {true, true, true, true}));
  return ResultPublication{take(builder.seal()), true};
}
struct Single {
  bool requested = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto port = phase.query.output_index;
    const auto shape =
        phase.query.inputs[port].result_schema->tensors[0].sample_shape();
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tensors.push_back({port, 0, take(Footprint::all(shape)), 1});
      return Result<ResultProgramPoll>(std::move(need));
    }
    double number = 0;
    check(phase.tensors->at({port, 0}).read({0}, &number, sizeof(number)));
    MutableBuffer scratch;
    if (shape[0] > 1)
      scratch = take(phase.allocator.allocate(shape[0] * 8));
    return Result<ResultProgramPoll>(publish(phase, number));
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
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    ++counts->joint_polls;
    if (counts->hook)
      counts->hook();
    ResourceVector<ResultJointOutcome> outcomes;
    if (failure == 4) {
      auto buffer = take(MutableValue::allocate(
          {ElementType::Float64, {2}}, Region::whole({2}), phase.allocator));
      const double numbers[2] = {7, 11};
      std::memcpy(buffer.data(), numbers, sizeof(numbers));
      auto backing = take(std::move(buffer).publish()).storage();
      for (const auto* member : phase.members) {
        const auto id = member->query.output_index;
        outcomes.push_back(
            {take(result_atom_key(member->query)),
             Result<ResultProgramPoll>(publish(*member, 0, backing, id * 8))});
      }
    } else {
      if (failure == 1)
        return Result<ResourceVector<ResultJointOutcome>>(
            Status{ErrorCode::BackendUnavailable, "unattributed failure"});
      if (failure == 2)
        return Result<ResourceVector<ResultJointOutcome>>(std::move(outcomes));
      for (const auto* member : phase.members) {
        const auto id = member->query.output_index;
        auto key = take(result_atom_key(member->query));
        if (failure == 3 && id == 0)
          outcomes.push_back(
              {key, Result<ResultProgramPoll>(Status{
                        ErrorCode::OperationFailed,
                        "local numerical error",
                        FailureReason::InvalidDomain,
                        {FailureOrigin::Domain, FailureScope::Atom, key}})});
        else
          outcomes.push_back({key, states[id].poll(*member)});
      }
    }
    return Result<ResourceVector<ResultJointOutcome>>(std::move(outcomes));
  }
};
OperationDefinition definition(std::shared_ptr<Counts> counts, int failure,
                               bool large) {
  OperationDefinition op;
  op.key = "test.joint_select";
  op.traits.input_count = 2;
  op.traits.input_schema.resize(2);
  for (auto& input : op.traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.result_schema_id = "test.multi_output";
    input.result_schema_version = 1;
  }
  op.traits.outputs.resize(2);
  op.traits.joint_contract = 1;
  op.traits.joint_continuation_bytes = large ? (1ULL << 30) : sizeof(Joint);
  for (unsigned i = 0; i < 2; ++i) {
    auto& output = op.traits.outputs[i];
    output = multi_result::output(i ? "right" : "left");
    output.key = i ? "right" : "left";
    output.input_indices = std::vector<std::uint32_t>{i};
    output.region_rule = OperationRegionRule::Dependency;
    output.continuation_bytes = sizeof(Single);
    output.maximum_dependency_stages = 3;
    output.failure_delivery = FailureDelivery::RequestFailureOnly;
  }
  op.start_result = [counts](const ResultProgramQuery& query,
                             const BufferAllocator& allocator) {
    ++counts->singles[query.output_index];
    return ResultContinuation::make<Single>(allocator);
  };
  op.start_result_joint = [counts, failure](const auto&,
                                            const BufferAllocator& allocator) {
    return ResultJointContinuation::make<Joint>(allocator, counts, failure);
  };
  return op;
}
WorkflowDocument document() {
  WorkflowDocument doc;
  doc.inputs = {multi_result::declaration(1, "a"),
                multi_result::declaration(2, "b")};
  doc.nodes = {{1,
                "test.joint_select",
                {WorkflowInputReference{1}, WorkflowInputReference{2}},
                {}}};
  doc.outputs = {{"left", 1, "left"}, {"right", 1, "right"}};
  return doc;
}
double number(const DemandResult& result, const std::string& key) {
  return multi_result::number(result.results.at(key.c_str()));
}
ExecutionBindings bindings_for(const ResourceBudget& root) {
  return ExecutionBindings{{multi_result::binding(root, "a", 7),
                            multi_result::binding(root, "b", 11)}};
}
int projected_request_record() {
  for (bool joint : {false, true}) {
    auto counts = std::make_shared<Counts>();
    auto registry = std::make_shared<OperationRegistry>();
    auto op = definition(counts, 0, false);
    op.traits.input_count = 3;
    op.traits.input_schema.push_back(op.traits.input_schema[0]);
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
    terminal.start_result_joint = {};
    auto record_calls = std::make_shared<std::atomic<unsigned>>(0);
    terminal.start_result = [record_calls](const ResultProgramQuery&,
                                           const BufferAllocator&) {
      ++*record_calls;
      return Result<ResultContinuation>(Status{
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
    auto frozen = execution
                      .freeze(compiled.value().plan,
                              bindings_for(take(execution.resource_budget())))
                      .take_value();
    ExecutionOptions options;
    options.enable_joint = joint;
    auto result = execution.execute_fragments(
        frozen,
        {{"left", Footprint::all({1}).take_value()},
         {"right", Footprint::all({1}).take_value()}},
        {}, options);
    if (!result.ok())
      std::cerr << "failure " << static_cast<unsigned>(result.status().code)
                << " reason " << static_cast<unsigned>(result.status().reason)
                << " " << result.status().message << '\n';
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
    const bool limited_checkpoint = scenario == 6;
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
    if (limited_checkpoint)
      config.maximum_result_checkpoint_scopes = 1;
    ExecutionContext execution(registry, config);
    auto bindings = bindings_for(take(execution.resource_budget()));
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
        auto next = take(execution.freeze(compiled.value().plan, bindings));
        auto right =
            execution.execute_fragments(next, {{"right", query.at("right")}});
        if (!right.ok())
          std::cerr << right.status().message << '\n';
        PS_CHECK(right.ok() && number(right.value(), "right") == 11);
        PS_CHECK(counts->singles[1] == 0 &&
                 right.value().diagnostics.cache_hits == 1);
      }
      continue;
    }
    if (!result.ok())
      std::cerr << "failure " << static_cast<unsigned>(result.status().code)
                << " reason " << static_cast<unsigned>(result.status().reason)
                << " " << result.status().message << '\n';
    PS_CHECK(result.ok());
    PS_CHECK(number(result.value(), "left") == 7 &&
             number(result.value(), "right") == 11);
    if (!disabled && !large && !failure) {
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
    auto next = take(execution.freeze(compiled.value().plan, bindings));
    auto again = execution.execute_fragments(next, query, {}, options);
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
  auto frozen =
      execution.freeze(plan, bindings_for(take(execution.resource_budget())))
          .take_value();
  auto left = execution.execute_fragments(
      frozen, {{"left", Footprint::all({1}).take_value()}});
  PS_CHECK(left.ok() && counts->singles[0] == 1 && counts->singles[1] == 0 &&
           counts->joint_starts == 0);
  auto next = take(
      execution.freeze(plan, bindings_for(take(execution.resource_budget()))));
  auto both = execution.execute_fragments(
      next, {{"left", Footprint::all({1}).take_value()},
             {"right", Footprint::all({1}).take_value()}});
  PS_CHECK(both.ok() && both.value().diagnostics.cache_hits == 1);
  PS_CHECK(counts->singles[0] == 1 && counts->singles[1] == 1 &&
           counts->joint_starts == 0);
  return 0;
}
int consumer_and_shared_owner() {
  for (unsigned scenario : {0U, 1U, 2U}) {
    const bool shared = scenario == 1, limited_work = scenario == 2;
    auto sum_polls = std::make_shared<std::atomic<unsigned>>(0);
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
    add.traits.input_schema = op.traits.input_schema;
    add.traits.outputs = {multi_result::output("value")};
    struct Sum {
      std::shared_ptr<std::atomic<unsigned>> polls;
      bool limited_work;
      bool requested = false;
      Sum(std::shared_ptr<std::atomic<unsigned>> polls, bool limited_work)
          : polls(std::move(polls)), limited_work(limited_work) {}
      Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
        ++*polls;
        if (!requested) {
          requested = true;
          ResultProgramNeed need;
          for (unsigned port = 0; port < 2; ++port)
            need.tensors.push_back({port, 0, take(Footprint::all({1})), 1});
          if (limited_work) {
            const auto used = phase.resources.statistics().issued.work;
            if (used > 100000 - 256)
              return Result<ResultProgramPoll>(
                  Status{ErrorCode::Internal, "fixture exhausted before Need"});
            check(phase.resources.consume({100000 - used - 256}));
          }
          return Result<ResultProgramPoll>(std::move(need));
        }
        double numbers[2] = {};
        for (unsigned port = 0; port < 2; ++port)
          check(phase.tensors->at({port, 0}).read({0}, &numbers[port], 8));
        auto builder = take(ResultBuilder::start(
            phase.resources, *phase.query.output.result_schema,
            phase.query.semantic_key));
        check(builder.bind_descriptor_relation(
            take(ResultRelation::cartesian(phase.resources, 1, {}))));
        const double value = numbers[0] + numbers[1];
        check(builder.publish_tensor(
            0, Region::whole({1}),
            {reinterpret_cast<const std::uint8_t*>(&value), 8},
            take(ResultRelation::unite(
                phase.resources,
                {take(ResultRelation::cartesian(
                     phase.resources, 1,
                     {0, 1, 0, 1, ResultSupportTarget::Tensor, 0})),
                 take(ResultRelation::cartesian(
                     phase.resources, 1,
                     {1, 1, 0, 1, ResultSupportTarget::Tensor, 0}))})),
            {true, true, true, true}));
        return Result<ResultProgramPoll>(
            ResultPublication{take(builder.seal()), true});
      }
    };
    add.start_result = [sum_polls, limited_work](const auto&,
                                                 const auto& allocator) {
      return ResultContinuation::make<Sum>(allocator, sum_polls, limited_work);
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
    ExecutionContextConfig config{2, false, 32, 4096, 1024};
    if (limited_work) {
      config.managed_resources = ResourceLimits{};
      config.managed_resources->maximum_work = 100000;
    }
    ExecutionContext execution(registry, config);
    auto frozen = execution
                      .freeze(compiled.value().plan,
                              bindings_for(take(execution.resource_budget())))
                      .take_value();
    DemandQuery query;
    for (const auto& output : doc.outputs)
      query.emplace(output.name, Footprint::all({1}).take_value());
    auto result = execution.execute_fragments(frozen, query);
    if (limited_work) {
      PS_CHECK(!result.ok() &&
               result.status().code == ErrorCode::ResourceExhausted &&
               result.status().reason == FailureReason::WorkLimit &&
               *sum_polls == 1 && counts->joint_starts == 0 &&
               counts->singles[0] == 0 && counts->singles[1] == 0);
      continue;
    }
    if (!result.ok())
      std::cerr << "failure " << static_cast<unsigned>(result.status().code)
                << " reason " << static_cast<unsigned>(result.status().reason)
                << " " << result.status().message << '\n';
    if (result.ok() && counts->joint_starts != 1)
      std::cerr << "consumer shared=" << shared
                << " groups=" << counts->joint_starts
                << " singles=" << counts->singles[0] << ","
                << counts->singles[1] << "\n";
    PS_CHECK(result.ok() && counts->joint_starts == 1);
    PS_CHECK(counts->singles[0] == 0 && counts->singles[1] == 0);
    if (shared) {
      const auto& a = result.value().results.at("left");
      const auto& b = result.value().results.at("right");
      auto left =
          take(a.acquire_tensor(take(a.descriptor()), 0, Region::whole({1})));
      auto right =
          take(b.acquire_tensor(take(b.descriptor()), 0, Region::whole({1})));
      PS_CHECK(left.storage_owner_token() &&
               left.storage_owner_token() == right.storage_owner_token());
      PS_CHECK(execution.cache_statistics().retained_bytes == 16);
      PS_CHECK(number(result.value(), "left") == 7 &&
               number(result.value(), "right") == 11);
    } else {
      PS_CHECK(number(result.value(), "sum") == 18);
    }
  }
  return 0;
}
int cancellation_with_external_waiter(bool peer_left, unsigned held_poll) {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->register_operation(definition(counts, 0, false)).ok());
  PS_CHECK(registry->freeze().ok());
  GraphContext graph(document());
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext execution(registry, {2, false, 32, 4096, 1024});
  auto frozen =
      execution.freeze(plan, bindings_for(take(execution.resource_budget())))
          .take_value();
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, release = false;
  counts->hook = [&] {
    if (counts->joint_polls != held_poll)
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
        frozen,
        {{peer_left ? "left" : "right", Footprint::all({1}).take_value()}});
  });
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!execution.cache_statistics().shared_computations &&
         std::chrono::steady_clock::now() < until)
    std::this_thread::yield();
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
    std::cerr << "peer left=" << peer_left << " poll=" << held_poll
              << " code=" << static_cast<unsigned>(b.status().code)
              << " origin=" << static_cast<unsigned>(b.status().detail.origin)
              << " scope=" << static_cast<unsigned>(b.status().detail.scope)
              << " " << b.status().message << '\n';
  PS_CHECK(subscribed && !a.ok() && a.status().code == ErrorCode::Cancelled);
  PS_CHECK(b.ok() && number(b.value(), peer_left ? "left" : "right") ==
                         (peer_left ? 7 : 11));
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
    ExecutionContextConfig config{2, false, 32, 1048576, 65536};
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Metadata] =
        64 * 1024 * 1024;
    ExecutionContext execution(registry, config);
    auto root = take(execution.resource_budget());
    ExecutionBindings bindings;
    if (deep) {
      for (std::uint64_t id = 2; id <= 64; ++id)
        doc.nodes.push_back({id,
                             op.key,
                             {WorkflowNodeOutput{id - 1, "left"},
                              WorkflowNodeOutput{id - 1, "right"}},
                             {}});
      doc.outputs = {{"left", 64, "left"}, {"right", 64, "right"}};
      bindings = bindings_for(root);
    } else {
      for (unsigned i = 0; i < 2; ++i) {
        auto schema = multi_result::schema(ElementType::Float64, {128});
        doc.inputs[i].result_schema =
            std::make_shared<const SchemaTemplate>(schema);
        bindings.inputs.push_back(
            multi_result::binding(root, i ? "b" : "a", i ? 11 : 7, schema));
      }
    }
    GraphContext graph(doc);
    auto compiled = Compiler(registry).compile(graph);
    PS_CHECK(compiled.ok());
    auto frozen =
        execution.freeze(compiled.value().plan, bindings).take_value();
    ExecutionOptions options;
    options.maximum_dependency_work = 64000000;
    options.maximum_dependency_cache_work = 64000000;
    options.dependencies.sets.maximum_work = 64000000;
    auto result = execution.execute_fragments(
        frozen,
        {{"left", Footprint::all({1}).take_value()},
         {"right", Footprint::all({1}).take_value()}},
        {}, options);
    if (!result.ok())
      std::cerr << "failure " << static_cast<unsigned>(result.status().code)
                << " reason " << static_cast<unsigned>(result.status().reason)
                << " " << result.status().message << '\n';
    if (!result.ok()) {
      const auto stats = root.statistics();
      std::cerr << "deep " << deep << " joints " << counts->joint_starts
                << " singles " << counts->singles[0] << ","
                << counts->singles[1] << " work " << stats.issued.work
                << " stages " << stats.issued.stages << " payload "
                << stats.peak[ResourceKind::Payload] << " metadata "
                << stats.peak[ResourceKind::Metadata] << "\n";
    }
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
  source.traits.outputs = {multi_result::output("value")};
  source.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  source.traits.outputs[0].continuation_bytes = sizeof(multi_result::Program);
  struct Constant {
    Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
      auto builder = take(ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key));
      check(builder.bind_descriptor_relation(
          take(ResultRelation::cartesian(phase.resources, 1, {}))));
      const double value = 7;
      check(builder.publish_tensor(
          0, Region::whole({1}),
          {reinterpret_cast<const std::uint8_t*>(&value), 8},
          take(ResultRelation::cartesian(phase.resources, 1, {})),
          {true, true, true, true}));
      return Result<ResultProgramPoll>(
          ResultPublication{take(builder.seal()), true});
    }
  };
  source.start_result = [](const ResultProgramQuery& query,
                           const BufferAllocator& allocator) {
    if (query.backend == Backend::Gpu)
      return Result<ResultContinuation>(
          Status{ErrorCode::BackendUnavailable, "test fallback"});
    return ResultContinuation::make<Constant>(allocator);
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
  options.execution_mode = ExecutionMode::NativeGpu;
  auto compiled = Compiler(registry).compile(graph, options);
  PS_CHECK(compiled.ok());
  ExecutionContext execution(registry, {2, false, 32, 65536, 1024});
  auto frozen = execution
                    .freeze(compiled.value().plan,
                            bindings_for(take(execution.resource_budget())))
                    .take_value();
  DemandQuery query{{"left", Footprint::all({1}).take_value()},
                    {"right", Footprint::all({1}).take_value()}};
  auto first = execution.execute_fragments(frozen, query);
  if (!first.ok())
    std::cerr << "fallback failure "
              << static_cast<unsigned>(first.status().code) << " reason "
              << static_cast<unsigned>(first.status().reason) << " "
              << first.status().message << '\n';

  PS_CHECK(first.ok() && counts->joint_starts == 1);
  auto next = take(execution.freeze(
      compiled.value().plan, bindings_for(take(execution.resource_budget()))));
  auto second = execution.execute_fragments(next, query);

  PS_CHECK(second.ok() && second.value().diagnostics.cache_hits == 1);
  PS_CHECK(counts->singles[0] == 1 && counts->singles[1] == 0);
  return 0;
}
int independent_joint_frontier() {
  auto left = std::make_shared<Counts>();
  auto right = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  std::mutex mutex;
  std::condition_variable changed;
  unsigned entered = 0;
  for (auto counts : {left, right}) {
    auto op = definition(counts, 0, false);
    op.key = counts == left ? "parallel.left_joint" : "parallel.right_joint";
    auto factory = op.start_result_joint;
    op.start_result_joint = [&, factory](const auto& query,
                                         const auto& allocator) {
      std::unique_lock<std::mutex> lock(mutex);
      ++entered;
      changed.notify_all();
      if (!changed.wait_for(lock, std::chrono::seconds(2),
                            [&] { return entered == 2; }))
        return Result<ResultJointContinuation>(Status{
            ErrorCode::OperationFailed, "independent joint start stalled"});
      lock.unlock();
      return factory(query, allocator);
    };
    check(registry->register_operation(std::move(op)));
  }
  struct Sum {
    bool requested = false;
    Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
      if (!requested) {
        requested = true;
        ResultProgramNeed need;
        for (unsigned port = 0; port < 4; ++port)
          need.tensors.push_back({port, 0, take(Footprint::all({1})), 1});
        return Result<ResultProgramPoll>(std::move(need));
      }
      double sum = 0;
      std::vector<ResultRelation> relations;
      for (unsigned port = 0; port < 4; ++port) {
        double value = 0;
        check(phase.read_tensor(port, 0, {0}, &value, sizeof(value)));
        sum += value;
        relations.push_back(take(ResultRelation::cartesian(
            phase.resources, 1,
            {port, 1, 0, 1, ResultSupportTarget::Tensor, 0})));
      }
      auto builder = take(ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {},
          std::vector<std::uint64_t>(phase.association->begin(),
                                     phase.association->end())));
      check(builder.bind_descriptor_relation(
          take(ResultRelation::cartesian(phase.resources, 1, {}))));
      check(builder.publish_tensor(
          0, Region::whole({1}),
          {reinterpret_cast<const std::uint8_t*>(&sum), sizeof(sum)},
          take(ResultRelation::unite(phase.resources, relations)),
          {true, true, true, true}));
      return Result<ResultProgramPoll>(
          ResultPublication{take(builder.seal()), true});
    }
  };
  OperationDefinition sum;
  sum.key = "parallel.sum_joints";
  sum.traits.input_count = 4;
  sum.traits.input_schema.resize(4);
  for (auto& input : sum.traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.tensor_key = "number";
  }
  sum.traits.outputs = {multi_result::output("value")};
  sum.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  sum.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<Sum>(allocator);
  };
  check(registry->register_operation(std::move(sum)));
  check(registry->freeze());
  auto doc = document();
  doc.nodes = {{1,
                "parallel.left_joint",
                {WorkflowInputReference{1}, WorkflowInputReference{2}},
                {}},
               {2,
                "parallel.right_joint",
                {WorkflowInputReference{1}, WorkflowInputReference{2}},
                {}},
               {3,
                "parallel.sum_joints",
                {WorkflowNodeOutput{1, "left"}, WorkflowNodeOutput{1, "right"},
                 WorkflowNodeOutput{2, "left"}, WorkflowNodeOutput{2, "right"}},
                {}}};
  doc.outputs = {{"sum", 3, "value"}};
  GraphContext graph(doc);
  auto compiled = take(Compiler(registry).compile(graph));
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.result_cache_bytes = 0;
  ExecutionContext context(registry, config);
  auto result = context.execute(compiled.plan,
                                bindings_for(take(context.resource_budget())));
  if (!result.ok())
    std::cerr << result.status().message << '\n';
  PS_CHECK(result.ok() &&
           multi_result::number(result.value().results.at("sum")) == 36);
  PS_CHECK(entered == 2 && result.value().diagnostics.joint_groups == 2);
  PS_CHECK(left->joint_starts == 1 && right->joint_starts == 1);
  PS_CHECK(left->singles[0] == 0 && left->singles[1] == 0 &&
           right->singles[0] == 0 && right->singles[1] == 0);
  return 0;
}
int rejected_joint_submission() {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(definition(counts, 0, false)));
  check(registry->freeze());
  GraphContext graph(document());
  auto compiled = take(Compiler(registry).compile(graph));
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Queue] = 0;
  ExecutionContext context(registry, config);
  auto result = context.execute(compiled.plan,
                                bindings_for(take(context.resource_budget())));
  PS_CHECK(!result.ok() &&
           result.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(counts->joint_starts == 0 && counts->singles[0] == 0 &&
           counts->singles[1] == 0);
  PS_CHECK(
      take(context.resource_budget()).statistics().live[ResourceKind::Queue] ==
      0);
  return 0;
}
}  // namespace

int main() try {
  PS_CHECK(independent_joint_frontier() == 0);
  PS_CHECK(rejected_joint_submission() == 0);
  PS_CHECK(projected_request_record() == 0);
  PS_CHECK(proportional_workspace_and_depth() == 0);
  PS_CHECK(fallback_ancestry() == 0);
  PS_CHECK(consumer_and_shared_owner() == 0);
  PS_CHECK(cancellation_with_external_waiter(false, 2) == 0);
  PS_CHECK(cancellation_with_external_waiter(true, 2) == 0);
  PS_CHECK(cancellation_with_external_waiter(true, 1) == 0);
  PS_CHECK(scenarios() == 0);
  PS_CHECK(independent_request_and_partial_hit() == 0);
  return 0;
}

catch (const std::exception& error) {
  std::cerr << error.what() << "\n";
  return 1;
}
