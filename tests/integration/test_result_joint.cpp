#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

#if defined(PHOTOSPIDER_JOINT_RETIREMENT_TEST)
#include "execution/execution_test_hooks.hpp"
#endif

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Counts {
  std::atomic<unsigned> starts{0}, destroys{0}, polls{0}, members{0};
  std::atomic<unsigned> single_starts{0}, single_destroys{0}, single_polls{0};
  std::function<void()> hook;
};
ResultProgramPoll publish(const ResultProgramPhase& phase, double value) {
  auto builder = take(ResultBuilder::start(phase.resources,
                                           *phase.query.output.result_schema,
                                           phase.query.semantic_key));
  auto relation = take(ResultRelation::cartesian(phase.resources, 1, {}));
  check(builder.bind_descriptor_relation(relation));
  const auto& tensor = phase.query.output.result_schema->tensors[0];
  auto demand = phase.query.tensor_outputs.value_or(
      take(Footprint::all(tensor.sample_shape())));
  auto tensor_relation =
      phase.results.count(0)
          ? take(ResultRelation::cartesian(
                phase.resources, take(tensor.sample_count()),
                {0, 1, 0, 1, ResultSupportTarget::Tensor, 0}))
          : take(ResultRelation::cartesian(phase.resources,
                                           take(tensor.sample_count()), {}));
  for (const auto& box : demand.boxes()) {
    std::vector<double> bytes(take(box.element_count()), value);
    check(builder.publish_tensor(
        0, box,
        ByteView(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                 bytes.size() * sizeof(value)),
        tensor_relation, {true, true, true, true}));
  }
  return ResultPublication{take(builder.seal()), true};
}
struct Shared {
  std::shared_ptr<Counts> counts;
  unsigned mode, stage = 0;
  Shared(std::shared_ptr<Counts> counts, unsigned mode)
      : counts(std::move(counts)), mode(mode) {}
  ~Shared() noexcept { ++counts->destroys; }
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    ++counts->polls;
    counts->members += phase.members.size();
    if (counts->hook)
      counts->hook();
    check(phase.consume_work(7));
    if (mode == 26 || mode == 28)
      return Result<ResourceVector<ResultJointOutcome>>(
          Status{ErrorCode::BackendUnavailable, "optional shared failure"});
    if (mode == 8) {
      static_cast<void>(phase.consume_work(1000000));
      throw std::runtime_error("later exception");
    }
    if (mode == 10) {
      static_cast<void>(TemporaryStorage::create(phase.members[0]->resources));
      return Result<ResourceVector<ResultJointOutcome>>(
          Status{ErrorCode::BackendUnavailable, "later backend failure"});
    }
    ResourceVector<ResultJointOutcome> outcomes;
    for (const auto* member : phase.members) {
      const auto index = member->query.output_index;
      const auto key = take(result_atom_key(member->query));
      if (mode == 7)
        static_cast<void>(phase.allocator.allocate(128));
      if (mode == 27 && stage == 0) {
        ResultProgramNeed need;
        const auto samples = take(Footprint::all({1}));
        for (unsigned i = 0; i < 4096; ++i)
          need.tensors.push_back({0, 0, samples, 8});
        outcomes.push_back({key, Result<ResultProgramPoll>(std::move(need))});
      } else if (mode >= 29 && stage == 0) {
        ResultProgramNeed need;
        need.results.push_back({0, 0, true, 0});
        outcomes.push_back({key, Result<ResultProgramPoll>(std::move(need))});
      } else if (index == 0 && stage == 0 &&
                 (mode == 0 || mode == 17 || mode == 18 || mode == 22)) {
        ResultProgramNeed need;
        need.io.push_back(ResultCreateTemporary{});
        need.results.push_back({mode == 18   ? 1U
                                : mode == 22 ? 99U
                                             : 0U,
                                0, true, 0});
        if (mode == 17) {
          ResourceBudget foreign;
          ResourceVector<ResultIoRequest> io{
              ResourceAllocator<ResultIoRequest>(foreign)};
          io.push_back(ResultCreateTemporary{});
          need.io = std::move(io);
        }
        outcomes.push_back({key, Result<ResultProgramPoll>(std::move(need))});
      } else if (index == 0 && mode == 5) {
        outcomes.push_back(
            {key, Result<ResultProgramPoll>(Status{
                      ErrorCode::OperationFailed,
                      "local domain",
                      FailureReason::DivideByZero,
                      {FailureOrigin::Domain, FailureScope::Unspecified}})});
      } else {
        if (index == 0 && mode == 0) {
          if (member->io.size() != 1 ||
              !std::get<TemporaryStorage>(member->io[0])
                   .owned_by(member->resources))
            return Result<ResourceVector<ResultJointOutcome>>(
                Status{ErrorCode::InvalidArgument, "missing I/O reply"});
        }
        const auto value =
            mode >= 29
                ? multi_result::number(member->results.at(0)) + 4 * (index + 1)
            : index == 0 && mode == 0
                ? multi_result::number(member->results.at(0)) + 4
                : 7 + 4 * index;
        auto publication = publish(*member, value);
        if (mode == 4 && index == 1)
          std::get<ResultPublication>(publication).complete = false;
        outcomes.push_back(
            {key, Result<ResultProgramPoll>(std::move(publication))});
      }
    }
    ++stage;
    if (mode == 16) {
      ResourceBudget foreign;
      ResourceVector<ResultJointOutcome> copy{
          ResourceAllocator<ResultJointOutcome>(foreign)};
      for (auto& outcome : outcomes)
        copy.push_back(std::move(outcome));
      outcomes = std::move(copy);
    }
    if (mode == 1)
      outcomes.pop_back();
    if (mode == 2)
      outcomes.back().key.output_index = outcomes.front().key.output_index;
    if (mode == 3)
      outcomes.back().key.output_index = 63;
    if (mode == 6)
      static_cast<void>(phase.consume_work(1000000));
    return Result<ResourceVector<ResultJointOutcome>>(std::move(outcomes));
  }
};
struct Singleton {
  std::shared_ptr<Counts> counts;
  unsigned mode;
  bool requested = false;
  Singleton(std::shared_ptr<Counts> counts, unsigned mode)
      : counts(std::move(counts)), mode(mode) {}
  ~Singleton() noexcept { ++counts->single_destroys; }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++counts->single_polls;
    const auto index = phase.query.output_index;
    if (mode == 5 && index == 0)
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed,
                 "local domain",
                 FailureReason::DivideByZero,
                 {FailureOrigin::Domain, FailureScope::Unspecified}});
    if (mode == 0 && index == 0 && !requested) {
      requested = true;
      ResultProgramNeed need;
      need.results.push_back({0, 0, true, 0});
      need.io.push_back(ResultCreateTemporary{});
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (mode == 28) {
      float value = 0;
      check(phase.read_tensor(0, 0, {0}, &value, sizeof(value)));
      if (value != 3)
        return Result<ResultProgramPoll>(Status{ErrorCode::TypeMismatch, {}});
    }
    const auto number = mode == 0 && index == 0
                            ? multi_result::number(phase.results.at(0)) + 4
                            : 7 + 4 * index;
    return Result<ResultProgramPoll>(publish(phase, number));
  }
};
OperationDefinition definition(std::shared_ptr<Counts> counts, unsigned mode) {
  OperationDefinition result;
  result.key = "test.result_joint";
  result.traits.outputs = {multi_result::output("left"),
                           multi_result::output("right")};
  OperationPortConstraint input;
  input.kind = OperationPortKind::Result;
  input.result_schema_id = "test.multi_output";
  input.result_schema_version = 1;
  result.traits.input_schema = {input};
  result.traits.input_count = 1;
  if (mode == 18) {
    result.traits.input_schema.push_back(input);
    result.traits.input_count = 2;
    result.traits.outputs[0].input_indices = std::vector<std::uint32_t>{0};
  }
  if (mode == 19 || mode == 20) {
    auto schema = multi_result::schema(ElementType::Float64, {2});
    schema.tensors[0].atomic_trailing_axes = mode == 20 ? 1 : 0;
    for (auto& output : result.traits.outputs)
      output.result_schema = schema;
  }
  if (mode == 28) {
    result.traits.input_schema[0].scalar_bounds = true;
    result.traits.input_schema[0].tensor_key = "number";
    result.traits.input_schema[0].minimum = 0;
    result.traits.input_schema[0].maximum = 100;
  }
  if (mode == 25)
    result.traits.outputs.push_back(multi_result::output("third"));
  result.traits.joint_contract = 1;
  result.traits.joint_continuation_bytes = 256;
  result.traits.joint_workspace_bytes = 64;
  result.start_result = [counts, mode](const auto&, const auto& allocator) {
    ++counts->single_starts;
    return ResultContinuation::make<Singleton>(allocator, counts, mode);
  };
  result.start_result_joint = [counts, mode](const auto& queries,
                                             const auto& allocator) {
    ++counts->starts;
    if (queries.size() > 1 && queries[0].prepared != queries[1].prepared)
      return Result<ResultJointContinuation>(
          Status{ErrorCode::Stale, "different joint preparation owners"});
    if (queries.empty())
      return Result<ResultJointContinuation>(
          Status{ErrorCode::InvalidArgument, {}});
    auto state = ResultJointContinuation::make<Shared>(allocator, counts, mode);
    if (mode == 9) {
      static_cast<void>(allocator.allocate(1024));
      throw std::runtime_error("later start exception");
    }
    return state;
  };
  return result;
}
int scenario(unsigned mode) {
  ResourceBudget root, external_queries;
  ResourceAllocationScope scope(root);
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(definition(counts, mode)));
  OperationMetadata source;
  source.result_schema =
      std::make_shared<const SchemaTemplate>(multi_result::schema());
  std::vector<OperationMetadata> sources{source};
  if (mode == 18)
    sources.push_back(source);
  auto inferred = take(infer_operation_outputs(
      take(registry->find_traits("test.result_joint")), sources, {}));
  ResultProgramMetadata left{sources, inferred[0]}, right{sources, inferred[1]};
  std::map<std::string, ParameterValue> parameters;
  ResourceVector<ResultProgramQuery> queries;
  queries.emplace_back(left, parameters);
  queries.emplace_back(right, parameters);
  for (unsigned i = 0; i < 2; ++i) {
    queries[i].output_index = i;
    queries[i].semantic_key = i ? "right" : "left";
    queries[i].snapshot_identity = "same-run";
    queries[i].tensor_outputs = take(Footprint::all(
        mode == 19 || mode == 20 ? std::vector<std::uint64_t>{2}
                                 : std::vector<std::uint64_t>{1}));
    if (mode == 20)
      queries[i].tensor_outputs =
          take(Footprint::from_regions({2}, {Region({{0, 1}})}));
  }
  if (mode == 24) {
    ResourceAllocationScope external_scope(external_queries);
    for (auto& query : queries)
      query.tensor_outputs = take(Footprint::all({1}));
    PS_CHECK(external_queries.statistics().live[ResourceKind::Metadata] > 0);
  }
  auto changed = queries;
  changed[1].output_index = 0;
  PS_CHECK(
      !registry->start_result_joint("test.result_joint", changed, root).ok());
  changed[1].output_index = 1;
  changed[1].snapshot_identity = "different-run";
  PS_CHECK(
      !registry->start_result_joint("test.result_joint", changed, root).ok());
  PS_CHECK(counts->starts == 0);
  changed.clear();
  CancellationSource cancellation;
  if (mode == 12) {
    queries[0].cancellation = cancellation.token();
    cancellation.cancel();
  }
  if (mode == 23) {
    queries[0].prepared = take(
        registry->prepare_operation("test.result_joint", sources, parameters));
    queries[1].prepared = take(
        registry->prepare_operation("test.result_joint", sources, parameters));
    PS_CHECK(queries[0].prepared != queries[1].prepared);
  }
  auto started =
      registry->start_result_joint("test.result_joint", queries, root);
  if (mode == 19) {
    PS_CHECK(!started.ok() &&
             started.status().code == ErrorCode::InvalidArgument &&
             counts->starts == 0);
    return 0;
  }
  if (mode == 9) {
    PS_CHECK(!started.ok() &&
             started.status().code == ErrorCode::ResourceExhausted);
    PS_CHECK(counts->starts == 1 && counts->destroys == 1);
    return 0;
  }
  auto state = take(std::move(started));
  if (mode == 24) {
    for (auto& query : queries)
      query.tensor_outputs = take(Footprint::all({1}));
    PS_CHECK(external_queries.statistics().live[ResourceKind::Metadata] == 0);
  }

  ResultObjectInputs inputs;
  ResourceVector<ResultIoReply> io_left, io_right;
  auto allocator = root.allocator();
  auto work = [&](std::uint64_t amount) {
    if (((mode == 6 || mode == 8) && amount > 7) || (mode == 22 && amount == 2))
      return Status{ErrorCode::ResourceExhausted, {}, FailureReason::WorkLimit};
    return root.consume(ResourceWork{amount});
  };
  auto failure_left = std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
  auto failure_right = std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
  const auto preflight_failure = Status{
      mode == 31 ? ErrorCode::Stale : ErrorCode::Cancelled,
      "member Need work stopped",
      FailureReason::None,
      {mode == 34 ? FailureOrigin::Protocol : FailureOrigin::Cancellation,
       mode == 32   ? FailureScope::Run
       : mode == 33 ? FailureScope::Group
                    : FailureScope::Unspecified}};
  auto right_work = [&](std::uint64_t amount) {
    return mode >= 30 ? preflight_failure : work(amount);
  };
  ResultProgramPhase phase_left{queries[0], inputs, io_left,     allocator,
                                root,       work,   failure_left};
  ResultProgramPhase phase_right{
      queries[1], inputs, io_right, allocator, root, right_work, failure_right};
  ResourceVector<const ResultProgramPhase*> ready{&phase_left, &phase_right};
  if (mode == 11)
    failure_left->store(ErrorCode::OperationFailed);
  if (mode == 13) {
    auto schema = std::make_shared<SchemaTemplate>(*left.output.result_schema);
    schema->id = "changed.schema";
    left.output.result_schema = std::move(schema);
  }
  if (mode == 14)
    queries[0].snapshot_identity = "changed.snapshot";
  bool reentrant_rejected = false;
  if (mode == 15) {
    counts->hook = [&] {
      auto nested = state.poll({ready, allocator, work});
      reentrant_rejected = !nested.ok() && nested.status().detail.origin ==
                                               FailureOrigin::Protocol;
    };
  }
  auto answer = [&] {
    if (mode != 21)
      return state.poll({ready, allocator, work});
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, released = false;
    counts->hook = [&] {
      std::unique_lock<std::mutex> lock(mutex);
      entered = true;
      condition.notify_all();
      condition.wait(lock, [&] { return released; });
    };
    std::optional<Result<ResourceVector<ResultJointOutcome>>> result;
    std::thread worker(
        [&] { result.emplace(state.poll({ready, allocator, work})); });
    {
      std::unique_lock<std::mutex> lock(mutex);
      condition.wait(lock, [&] { return entered; });
    }
    auto concurrent = state.poll({ready, allocator, work});
    reentrant_rejected =
        !concurrent.ok() &&
        concurrent.status().detail.origin == FailureOrigin::Protocol;
    {
      std::lock_guard<std::mutex> lock(mutex);
      released = true;
    }
    condition.notify_all();
    worker.join();
    counts->hook = {};
    return std::move(*result);
  }();
  if (mode >= 32) {
    PS_CHECK(!answer.ok() && answer.status().code == preflight_failure.code &&
             answer.status().detail.origin == preflight_failure.detail.origin &&
             answer.status().detail.scope == preflight_failure.detail.scope);
    auto repeated = state.poll({ready, allocator, work});
    PS_CHECK(!repeated.ok() &&
             repeated.status().detail.scope == preflight_failure.detail.scope);
  } else if ((mode >= 1 && mode <= 4) || (mode >= 16 && mode <= 18)) {
    PS_CHECK(!answer.ok() &&
             answer.status().detail.origin == FailureOrigin::Protocol);
  } else if (mode == 6 || mode == 7 || mode == 8 || mode == 22) {
    PS_CHECK(!answer.ok() &&
             answer.status().code == ErrorCode::ResourceExhausted);
  } else if (mode == 10) {
    PS_CHECK(!answer.ok() &&
             answer.status().reason == FailureReason::UnauthorizedRead &&
             answer.status().detail.origin == FailureOrigin::Protocol);
  } else if (mode == 13 || mode == 14) {
    PS_CHECK(!answer.ok() && counts->polls == 0);
    PS_CHECK(answer.status().code ==
             (mode == 13 ? ErrorCode::Stale : ErrorCode::InvalidArgument));
  } else {
    auto outcomes = take(std::move(answer));
    std::sort(outcomes.begin(), outcomes.end(),
              [](const auto& a, const auto& b) { return a.key < b.key; });
    PS_CHECK(outcomes.size() == 2);
    if (mode == 30 || mode == 31) {
      PS_CHECK(outcomes[0].outcome.ok() &&
               std::holds_alternative<ResultProgramNeed>(
                   outcomes[0].outcome.value()));
      PS_CHECK(!outcomes[1].outcome.ok() &&
               outcomes[1].outcome.status().code == preflight_failure.code &&
               !outcomes[1].quality);
      inputs[0] = multi_result::binding(root, "source", 3).result;
      ready.resize(1);
      auto completed = take(state.poll({ready, allocator, work}));
      PS_CHECK(completed.size() == 1 && completed[0].outcome.ok());
      PS_CHECK(multi_result::number(
                   std::get<ResultPublication>(completed[0].outcome.value())
                       .result) == 7);
      state = {};
      PS_CHECK(counts->destroys == 1 && counts->starts == 1);
      return 0;
    } else if (mode == 5) {
      PS_CHECK(!outcomes[0].outcome.ok() &&
               outcomes[0].outcome.status().reason ==
                   FailureReason::DivideByZero);
    } else if (mode == 11 || mode == 12) {
      PS_CHECK(
          !outcomes[0].outcome.ok() &&
          outcomes[0].outcome.status().code ==
              (mode == 11 ? ErrorCode::OperationFailed : ErrorCode::Cancelled));
      PS_CHECK(counts->members == 1);
    } else if (mode == 15 || mode == 20 || mode == 21 || mode == 23 ||
               mode == 24) {
      PS_CHECK(((mode != 15 && mode != 21) || reentrant_rejected) &&
               counts->polls == 1);
      if (mode == 20)
        PS_CHECK(take(std::get<ResultPublication>(outcomes[0].outcome.value())
                          .result.descriptor())
                     .tensor_coverage(0) == take(Footprint::all({2})));
      PS_CHECK(multi_result::number(
                   std::get<ResultPublication>(outcomes[0].outcome.value())
                       .result) == 7);
    } else {
      PS_CHECK(std::holds_alternative<ResultProgramNeed>(
          outcomes[0].outcome.value()));
      io_left.push_back(take(TemporaryStorage::create(root)));
      inputs[0] = multi_result::binding(root, "source", 3).result;
      ready.resize(1);
      auto second = take(state.poll({ready, allocator, work}));
      PS_CHECK(second.size() == 1 && second[0].key.output_index == 0);
      PS_CHECK(
          multi_result::number(
              std::get<ResultPublication>(second[0].outcome.value()).result) ==
          7);
      PS_CHECK(counts->polls == 2 && counts->members == 3);
      auto retired = state.poll({ready, allocator, work});
      PS_CHECK(!retired.ok() &&
               retired.status().detail.origin == FailureOrigin::Protocol);
    }
    auto result =
        std::get<ResultPublication>(outcomes[1].outcome.value()).result;
    PS_CHECK(multi_result::number(result) == 11);
    registry.reset();
    state = {};
    PS_CHECK(counts->destroys == 1 && counts->starts == 1);
    PS_CHECK(multi_result::number(result) == 11);
  }
  if (!answer.ok()) {
    const auto prior_polls = counts->polls.load();
    auto repeated = state.poll({ready, allocator, work});
    PS_CHECK(!repeated.ok() && repeated.status().code == answer.status().code &&
             counts->polls == prior_polls);
    state = {};
    PS_CHECK(counts->destroys == 1);
  }
  return 0;
}
WorkflowDocument document() {
  WorkflowDocument result;
  result.inputs = {multi_result::declaration(1, "source")};
  result.nodes = {{10, "test.result_joint", {WorkflowInputReference{1}}, {}}};
  result.outputs = {{"left", 10, "left"}, {"right", 10, "right"}};
  return result;
}
int workflow() {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(definition(counts, 0)));
  check(registry->freeze());
  auto doc = document();
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  PS_CHECK(plan.execution_groups().size() == 1);
  ExecutionContext execution(registry, {1, false, 16, 4096, 2048});
  auto root = take(execution.resource_budget());
  ExecutionBindings bindings{{multi_result::binding(root, "source", 3)}};
  auto frozen = take(execution.freeze(plan, bindings));
  const auto point = take(Footprint::all({1}));
  DemandQuery queries{{"left", point}, {"right", point}};
  auto first = execution.execute_fragments(frozen, queries);
  if (!first.ok())
    std::cerr << "joint workflow: " << first.status().message << '\n';
  PS_CHECK(first.ok());
  PS_CHECK(multi_result::number(first.value().results.at("left")) == 7);
  PS_CHECK(multi_result::number(first.value().results.at("right")) == 11);
  PS_CHECK(first.value().diagnostics.joint_groups == 1 &&
           first.value().diagnostics.joint_polls == 2);
  PS_CHECK(counts->starts == 1 && counts->destroys == 1 &&
           counts->single_starts == 0 && counts->polls == 2 &&
           counts->members == 3);
  auto dirty =
      take(first.value().dependencies.potential_dirty("source", point));
  PS_CHECK(dirty.at("left") == point && dirty.at("right").empty());
  bindings.inputs[0] = multi_result::binding(root, "source", 3);
  auto equivalent = take(execution.freeze(plan, bindings));
  auto warm = take(execution.execute_fragments(equivalent, queries));
  PS_CHECK(warm.diagnostics.cache_hits == 2 && counts->starts == 1 &&
           counts->single_starts == 0);
  bindings.inputs[0] = multi_result::binding(root, "source", 13);
  auto changed = take(execution.freeze(plan, bindings));
  auto partial = take(execution.execute_fragments(changed, queries));
  PS_CHECK(multi_result::number(partial.results.at("left")) == 17 &&
           multi_result::number(partial.results.at("right")) == 11);
  PS_CHECK(partial.diagnostics.cache_hits == 1 && counts->starts == 1 &&
           counts->single_starts == 1 && counts->single_destroys == 1);
  return 0;
}
int workflow_failure(unsigned mode, bool reversed = false) {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(definition(counts, mode)));
  check(registry->freeze());
  auto doc = document();
  if (reversed)
    doc.outputs = {{"a_right", 10, "right"}, {"z_left", 10, "left"}};
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext execution(registry, {1, false, 16, 4096, 2048});
  auto root = take(execution.resource_budget());
  auto frozen = take(execution.freeze(
      plan, ExecutionBindings{{multi_result::binding(root, "source", 3)}}));
  auto point = take(Footprint::all({1}));
  DemandQuery queries;
  for (const auto& named : doc.outputs)
    queries.emplace(named.name, point);
  std::vector<std::uint32_t> delivered;
  ExecutionOptions options;
  options.result_publication = [&](ValueRef result, const ResultRef&) {
    delivered.push_back(result.output_index);
    return Status::success();
  };
  auto failed = execution.execute_fragments(frozen, queries, {}, options);
  PS_CHECK(!failed.ok() && counts->starts == 1 && counts->destroys == 1 &&
           counts->single_starts == 0);
  if (mode == 5) {
    PS_CHECK(failed.status().reason == FailureReason::DivideByZero);
    PS_CHECK(delivered == (reversed ? std::vector<std::uint32_t>{1}
                                    : std::vector<std::uint32_t>{}));
    const auto name = reversed ? "a_right" : "right";
    auto equivalent = take(execution.freeze(
        plan, ExecutionBindings{{multi_result::binding(root, "source", 3)}}));
    auto healthy =
        take(execution.execute_fragments(equivalent, {{name, point}}));
    PS_CHECK(multi_result::number(healthy.results.at(name)) == 11 &&
             healthy.diagnostics.cache_hits == 1 && counts->single_starts == 0);
  } else {
    PS_CHECK(failed.status().detail.origin == FailureOrigin::Protocol &&
             delivered.empty());
  }
  return 0;
}
int workflow_single(unsigned admission) {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(definition(counts, 0)));
  check(registry->freeze());
  auto doc = document();
  if (!admission)
    doc.outputs = {{"right", 10, "right"}};
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext execution(registry, {1, false, 16, 4096, 2048});
  auto root = take(execution.resource_budget());
  auto frozen = take(execution.freeze(
      plan, ExecutionBindings{{multi_result::binding(root, "source", 3)}}));
  auto point = take(Footprint::all({1}));
  DemandQuery queries;
  for (const auto& named : doc.outputs)
    queries.emplace(named.name, point);
  ExecutionOptions options;
  if (admission == 1)
    options.dependencies.maximum_state_bytes = 64;
  if (admission == 2)
    options.enable_joint = false;
  auto result = take(execution.execute_fragments(frozen, queries, {}, options));
  PS_CHECK(multi_result::number(result.results.at("right")) == 11 &&
           counts->starts == 0 && counts->single_starts == (admission ? 2 : 1));
  if (admission)
    PS_CHECK(multi_result::number(result.results.at("left")) == 7);
  return 0;
}

int workflow_extra(unsigned mode, bool empty_first) {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(definition(counts, mode)));
  check(registry->freeze());
  auto doc = document();
  auto input_schema = multi_result::schema(mode == 28 ? ElementType::Float32
                                                      : ElementType::Float64);
  doc.inputs = {multi_result::declaration(1, "source", input_schema)};
  if (mode == 25)
    doc.outputs.push_back({"third", 10, "third"});
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext execution(registry, {1, false, 16, 4096, 0});
  auto root = take(execution.resource_budget());
  auto frozen =
      take(execution.freeze(plan, ExecutionBindings{{multi_result::binding(
                                      root, "source", 3, input_schema)}}));
  auto point = take(Footprint::all({1}));
  DemandQuery queries;
  for (const auto& named : doc.outputs)
    queries.emplace(named.name, empty_first && named.name == "left"
                                    ? take(Footprint::none({1}))
                                    : point);
  auto result = take(execution.execute_fragments(frozen, queries));
  PS_CHECK(multi_result::number(result.results.at("right")) == 11);
  if (mode == 25) {
    PS_CHECK(multi_result::number(result.results.at("third")) == 15);
    PS_CHECK(counts->starts == 1 &&
             counts->single_starts == (empty_first ? 1 : 0));
  } else {
    PS_CHECK(multi_result::number(result.results.at("left")) == 7);
    PS_CHECK(counts->starts == 1 && counts->destroys == 1 &&
             counts->single_starts == 2 && counts->single_destroys == 2);
    if (mode == 28) {
      auto dirty = take(result.dependencies.potential_dirty("source", point));
      PS_CHECK(dirty.at("left") == point && dirty.at("right") == point);
    }
    PS_CHECK(result.diagnostics.joint_groups == 1 &&
             result.diagnostics.joint_polls == 1 &&
             result.diagnostics.joint_fallbacks == 1);
  }
  return 0;
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, released = false, timed_out = false;
  void hold() {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    changed.notify_all();
    if (!changed.wait_for(lock, std::chrono::seconds(5),
                          [&] { return released; }))
      timed_out = true;
  }
  bool wait() {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(5),
                            [&] { return entered; });
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
};
#if defined(PHOTOSPIDER_JOINT_RETIREMENT_TEST)
Gate* retirement_gate = nullptr;
std::atomic<unsigned> retired_callbacks{0};
bool reject_adoption = false;
std::atomic<unsigned> handoff_attempts{0};
void hold_retirement() noexcept {
  if (++retired_callbacks == 2)
    retirement_gate->hold();
}
void reject_handoff() {
  ++handoff_attempts;
  if (reject_adoption)
    throw std::bad_alloc();
}
const ResourceBudget* denied_root = nullptr;
ResourceLease retirement_pressure;
bool denied_retirement = false;
void exhaust_retirement() noexcept {
  if (++retired_callbacks != 2)
    return;
  auto available = denied_root->available_capacity()[ResourceKind::Host];
  auto overhead = ResourceBudget::lease_metadata_bytes();
  if (available <= overhead)
    return;
  auto pressure =
      denied_root->reserve(ResourceCapacity::host(available - overhead));
  if (pressure.ok()) {
    retirement_pressure = pressure.take_value();
    denied_retirement = true;
  }
}
int workflow_retirement_allocation() {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(definition(counts, 25)));
  check(registry->freeze());
  auto doc = document();
  doc.outputs.push_back({"third", 10, "third"});
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContextConfig config{1, false, 16, 4096, 0};
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Host] = 1048576;
  ExecutionContext execution(registry, config);
  auto root = take(execution.resource_budget());
  auto frozen = take(execution.freeze(
      plan, ExecutionBindings{{multi_result::binding(root, "source", 3)}}));
  auto point = take(Footprint::all({1}));
  execution_testing::ExecutionTestHooks hooks;
  hooks.callback_body_finished = exhaust_retirement;
  denied_root = &root;
  retired_callbacks = 0;
  denied_retirement = false;
  execution_testing::install_execution_test_hooks(&hooks);
  auto failed = execution.execute_fragments(
      frozen, {{"left", point}, {"right", point}, {"third", point}});
  execution_testing::install_execution_test_hooks(nullptr);
  denied_root = nullptr;
  retirement_pressure = {};
  PS_CHECK(denied_retirement && !failed.ok() &&
           failed.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(counts->starts == 1 && counts->destroys == 1 &&
           execution.cache_statistics().in_flight == 0 &&
           root.statistics().live[ResourceKind::Queue] == 0 &&
           root.statistics().quarantined[ResourceKind::Host] == 0);
  auto recovered =
      take(execution.execute_fragments(frozen, {{"right", point}}));
  PS_CHECK(multi_result::number(recovered.results.at("right")) == 11);
  return 0;
}
#endif
int workflow_peer(bool retirement = false, bool reject = false) {
  auto counts = std::make_shared<Counts>();
  Gate gate;
  counts->hook = [&] {
    if (!retirement)
      gate.hold();
  };
#if defined(PHOTOSPIDER_JOINT_RETIREMENT_TEST)
  execution_testing::ExecutionTestHooks hooks;
  if (retirement) {
    retirement_gate = &gate;
    retired_callbacks = 0;
    reject_adoption = reject;
    handoff_attempts = 0;
    hooks.callback_body_finished = hold_retirement;
    hooks.structured_handoff_ready = reject_handoff;
    execution_testing::install_execution_test_hooks(&hooks);
  }
#else
  static_cast<void>(reject);
#endif
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(definition(counts, 25)));
  check(registry->freeze());
  auto doc = document();
  doc.outputs.push_back({"third", 10, "third"});
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext execution(registry, {1, false, 16, 4096, 0});
  auto root = take(execution.resource_budget());
  auto frozen = take(execution.freeze(
      plan, ExecutionBindings{{multi_result::binding(root, "source", 3)}}));
  auto point = take(Footprint::all({1}));
  CancellationSource cancelled;
  auto first = std::async(std::launch::async, [&] {
    return execution.execute_fragments(
        frozen, {{"left", point}, {"right", point}, {"third", point}},
        cancelled.token());
  });
  const bool entered = gate.wait();
  auto peer = std::async(std::launch::async, [&] {
    return execution.execute_fragments(frozen, {{"right", point}});
  });
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!execution.cache_statistics().shared_computations &&
         std::chrono::steady_clock::now() < until)
    std::this_thread::yield();
  const bool joined = execution.cache_statistics().shared_computations != 0;
  cancelled.cancel();
  bool observed_handoff = false;
  bool observed_drain = false;
  bool rejecting = false;
#if defined(PHOTOSPIDER_JOINT_RETIREMENT_TEST)
  rejecting = retirement && reject;
#endif
  if (rejecting) {
#if defined(PHOTOSPIDER_JOINT_RETIREMENT_TEST)
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!handoff_attempts && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    observed_handoff = handoff_attempts == 1;
    observed_drain = first.wait_for(std::chrono::milliseconds(20)) !=
                     std::future_status::ready;
#endif
  } else {
    observed_handoff =
        first.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    observed_drain = true;
  }
  gate.release();
  auto initial = first.get();
  auto healthy = peer.get();
#if defined(PHOTOSPIDER_JOINT_RETIREMENT_TEST)
  execution_testing::install_execution_test_hooks(nullptr);
  retirement_gate = nullptr;
#endif
  PS_CHECK(entered && joined && !gate.timed_out && observed_handoff &&
           observed_drain);
#if defined(PHOTOSPIDER_JOINT_RETIREMENT_TEST)
  if (retirement)
    PS_CHECK(handoff_attempts == 1);
#endif
  if (!healthy.ok())
    std::cerr << "joint peer: " << healthy.status().message << '\n';
  PS_CHECK(!initial.ok() && initial.status().code == ErrorCode::Cancelled);
  PS_CHECK(healthy.ok() &&
           multi_result::number(healthy.value().results.at("right")) == 11);
  PS_CHECK(counts->starts == 1 && counts->destroys == 1 &&
           counts->single_starts == 0 &&
           execution.cache_statistics().in_flight == 0);
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}

int workflow_need_budget() {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(definition(counts, 27)));
  check(registry->freeze());
  auto doc = document();
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext execution(registry, {1, false, 16, 4096, 0});
  auto root = take(execution.resource_budget());
  auto frozen = take(execution.freeze(
      plan, ExecutionBindings{{multi_result::binding(root, "source", 3)}}));
  auto point = take(Footprint::all({1}));
  ExecutionOptions options;
  options.maximum_dependency_work = 1000;
  auto failed = execution.execute_fragments(
      frozen, {{"left", point}, {"right", point}}, {}, options);
  PS_CHECK(!failed.ok() &&
           failed.status().code == ErrorCode::ResourceExhausted &&
           failed.status().detail.scope == FailureScope::Run);
  PS_CHECK(counts->starts == 1 && counts->destroys == 1 &&
           counts->single_starts == 0 && counts->polls == 1);
  return 0;
}

int workflow_nested() {
  auto counts = std::make_shared<Counts>();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(definition(counts, 29)));
  check(registry->freeze());
  auto doc = document();
  doc.nodes.push_back(
      {20, "test.result_joint", {WorkflowNodeOutput{10, "left"}}, {}});
  doc.outputs = {{"a_nested_left", 20, "left"},
                 {"b_nested_right", 20, "right"},
                 {"z_upstream_right", 10, "right"}};
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext execution(registry, {1, false, 16, 4096, 0});
  auto root = take(execution.resource_budget());
  auto frozen = take(execution.freeze(
      plan, ExecutionBindings{{multi_result::binding(root, "source", 3)}}));
  auto point = take(Footprint::all({1}));
  DemandQuery queries;
  for (const auto& named : doc.outputs)
    queries.emplace(named.name, point);
  auto result = execution.execute_fragments(frozen, queries);
  if (!result.ok())
    std::cerr << "nested joint: " << result.status().message << '\n';
  PS_CHECK(result.ok());
  PS_CHECK(
      multi_result::number(result.value().results.at("a_nested_left")) == 11 &&
      multi_result::number(result.value().results.at("b_nested_right")) == 15 &&
      multi_result::number(result.value().results.at("z_upstream_right")) ==
          11);
  PS_CHECK(counts->starts == 2 && counts->destroys == 2 &&
           counts->single_starts == 0 &&
           result.value().diagnostics.joint_groups == 2);
  auto dirty =
      take(result.value().dependencies.potential_dirty("source", point));
  for (const auto& output : dirty)
    PS_CHECK(output.second == point);
  return 0;
}

}  // namespace
int main() try {
#if defined(PHOTOSPIDER_JOINT_RETIREMENT_TEST)
  PS_CHECK(workflow_peer(true, false) == 0);
  PS_CHECK(workflow_peer(true, true) == 0);
  PS_CHECK(workflow_retirement_allocation() == 0);
#endif
  for (unsigned mode = 0; mode <= 24; ++mode)
    PS_CHECK(scenario(mode) == 0);
  for (unsigned mode = 30; mode <= 34; ++mode)
    PS_CHECK(scenario(mode) == 0);
  PS_CHECK(workflow() == 0);
  PS_CHECK(workflow_single(false) == 0);
  PS_CHECK(workflow_single(1) == 0);
  PS_CHECK(workflow_single(2) == 0);
  PS_CHECK(workflow_extra(25, false) == 0);
  PS_CHECK(workflow_extra(25, true) == 0);
  PS_CHECK(workflow_extra(26, false) == 0);
  PS_CHECK(workflow_extra(28, false) == 0);
  PS_CHECK(workflow_need_budget() == 0);
  PS_CHECK(workflow_peer() == 0);
  PS_CHECK(workflow_nested() == 0);
  for (unsigned mode : {1U, 2U, 3U, 4U, 5U, 17U})
    PS_CHECK(workflow_failure(mode) == 0);
  PS_CHECK(workflow_failure(5, true) == 0);
  return 0;
} catch (const multi_result::Failure& failure) {
  std::cerr << static_cast<int>(failure.status.code) << ": " << failure.what()
            << '\n';
  return 1;
}
