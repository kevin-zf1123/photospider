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
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
ResultProgramPoll publication(const ResultProgramPhase& phase, double value,
                              ResultSupport input = {},
                              ResultSupport descriptor_support = {}) {
  auto builder = take(ResultBuilder::start(phase.resources,
                                           *phase.query.output.result_schema,
                                           phase.query.semantic_key));
  check(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(phase.resources, 1, descriptor_support))));
  const auto& tensor = phase.query.output.result_schema->tensors[0];
  auto coverage = phase.query.tensor_outputs.value_or(
      take(Footprint::all(tensor.sample_shape())));
  auto relation = take(ResultRelation::cartesian(
      phase.resources, take(tensor.sample_count()), input));
  for (const auto& box : coverage.boxes()) {
    if (take(box.element_count()) != 1)
      throw std::runtime_error("source query is not one sample");
    check(builder.publish_tensor(
        0, box, {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)},
        relation, {true, true, true, true}));
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
    auto at = phase.query.tensor_outputs->boxes()[0].dimensions()[0].offset;
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
  definition.traits.outputs = {multi_result::output(
      "value", multi_result::schema(ElementType::Float64, {width}))};
  definition.start_result = [calls, hook](const auto&, const auto& allocator) {
    return ResultContinuation::make<Source>(allocator, calls, hook);
  };
  return definition;
}
struct Pair {
  bool requested = false;
  bool metadata;
  explicit Pair(bool metadata) : metadata(metadata) {}
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    ResourceVector<ResultJointOutcome> outcomes;
    for (const auto* member : phase.members) {
      const auto at = member->query.output_index;
      const auto key = take(result_atom_key(member->query));
      if (!requested) {
        ResultProgramNeed need;
        need.tensors.push_back(
            {0, 0, take(Footprint::from_regions({2}, {Region({{at, 1}})})),
             metadata ? 9U : 1U});
        outcomes.push_back({key, Result<ResultProgramPoll>(std::move(need))});
      } else {
        double value = 0;
        check(member->tensors->at({0, 0}).read({at}, &value, sizeof(value)));
        outcomes.push_back(
            {key,
             Result<ResultProgramPoll>(publication(
                 *member, value,
                 ResultSupport{0, 1, at, 1, ResultSupportTarget::Tensor, 0}))});
      }
    }
    requested = true;
    return Result<ResourceVector<ResultJointOutcome>>(std::move(outcomes));
  }
};
struct SinglePair {
  bool requested = false, metadata;
  explicit SinglePair(bool metadata) : metadata(metadata) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto at = phase.query.output_index;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tensors.push_back(
          {0, 0, take(Footprint::from_regions({2}, {Region({{at, 1}})})),
           metadata ? 9U : 1U});
      return Result<ResultProgramPoll>(std::move(need));
    }
    double value = 0;
    check(phase.tensors->at({0, 0}).read({at}, &value, sizeof(value)));
    return Result<ResultProgramPoll>(publication(
        phase, value, {0, 1, at, 1, ResultSupportTarget::Tensor, 0}));
  }
};
struct Sum {
  unsigned width, next = 0;
  double total = 0;
  explicit Sum(unsigned width) : width(width) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (next) {
      double value = 0;
      check(phase.tensors->at({0, 0}).read({next - 1}, &value, sizeof(value)));
      total += value;
    }
    if (next < width) {
      ResultProgramNeed need;
      need.tensors.push_back(
          {0, 0,
           take(Footprint::from_regions({width}, {Region({{next++, 1}})})), 1});
      return Result<ResultProgramPoll>(std::move(need));
    }
    return Result<ResultProgramPoll>(publication(
        phase, total,
        ResultSupport{0, 1, 0, width, ResultSupportTarget::Tensor, 0}));
  }
};
OperationDefinition consumer_definition(bool pair, unsigned width,
                                        bool metadata = false) {
  OperationDefinition definition;
  definition.key = "queries.consumer";
  definition.traits.input_count = 1;
  definition.traits.input_schema.resize(1);
  definition.traits.input_schema[0].kind = OperationPortKind::Result;
  definition.traits.input_schema[0].result_schema_id = "test.multi_output";
  definition.traits.input_schema[0].result_schema_version = 1;
  definition.traits.outputs = {multi_result::output(pair ? "left" : "value")};
  definition.traits.outputs[0].maximum_dependency_stages = width + 1;
  if (pair)
    definition.traits.outputs.push_back(multi_result::output("right"));
  definition.start_result = [pair, width, metadata](const auto&,
                                                    const auto& allocator) {
    return pair ? ResultContinuation::make<SinglePair>(allocator, metadata)
                : ResultContinuation::make<Sum>(allocator, width);
  };
  if (pair) {
    definition.traits.joint_contract = 1;
    definition.traits.joint_continuation_bytes = 512;
    definition.traits.joint_workspace_bytes = 4096;
    definition.start_result_joint = [metadata](const auto&,
                                               const auto& allocator) {
      return ResultJointContinuation::make<Pair>(allocator, metadata);
    };
  }
  return definition;
}
int workflow(bool pair, bool frozen) {
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
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 16, 1024 * 1024, 0});
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
  auto result = frozen
                    ? context.execute(take(context.freeze(plan)), {}, options)
                    : context.execute(plan, {}, {}, options);
  if (!result.ok())
    std::cerr << "query workflow: " << result.status().message << '\n';
  PS_CHECK(result.ok() && *calls == width && notices == width);
  if (pair) {
    PS_CHECK(multi_result::number(result.value().results.at("left")) == 10);
    PS_CHECK(multi_result::number(result.value().results.at("right")) == 11);
    PS_CHECK(result.value().diagnostics.joint_groups == 1 &&
             result.value().diagnostics.joint_fallbacks == 0);
  } else {
    PS_CHECK(multi_result::number(result.value().results.at("total")) ==
             width * 10 + width * (width - 1) / 2);
    PS_CHECK(previous_live == 0);
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
        phase.query.tensor_outputs->boxes()[0].dimensions()[0].offset;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tensors.push_back({0, 0, *phase.query.tensor_outputs, 2});
      return Result<ResultProgramPoll>(std::move(need));
    }
    ++*calls;
    double control = 0;
    check(phase.tensors->at({0, 0}).read({at}, &control, sizeof(control)));
    return Result<ResultProgramPoll>(
        publication(phase, (control > 0 ? 10 : 20) + at, {},
                    {0, 2, at, 1, ResultSupportTarget::Tensor, 0}));
  }
};
OperationDefinition controlled_definition(
    const std::shared_ptr<std::atomic<unsigned>>& calls) {
  auto source = source_definition(2, calls);
  source.traits.input_count = 1;
  source.traits.input_schema.resize(1);
  source.traits.input_schema[0].kind = OperationPortKind::Result;
  source.traits.input_schema[0].result_schema_id = "test.multi_output";
  source.traits.input_schema[0].result_schema_version = 1;
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
  const auto schema = multi_result::schema(ElementType::Float64, {2});
  doc.inputs = {multi_result::declaration(1, "control", schema)};
  doc.nodes = {{10, "queries.source", {WorkflowInputReference{1}}, {}},
               {20, "queries.consumer", {WorkflowNodeOutput{10, "value"}}, {}}};
  doc.outputs = {{"left", 20, "left"}, {"right", 20, "right"}};
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContextConfig config{1, false, 16, 1024 * 1024, 1048576};
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  auto frozen = take(context.freeze(
      plan,
      ExecutionBindings{{multi_result::binding(root, "control", 1, schema)}}));
  const auto one = take(Footprint::all({1}));
  const DemandQuery wanted{{"left", one}, {"right", one}};
  const auto verify = [&](const DemandResult& result) {
    for (unsigned at = 0; at < 2; ++at) {
      auto point = take(Footprint::from_regions({2}, {Region({{at, 1}})}));
      auto dirty = take(result.dependencies.potential_dirty(
          "control", point, 2, {}, ResultSupportTarget::Tensor, 0));
      check(dirty.at(at ? "right" : "left") == one &&
                    dirty.at(at ? "left" : "right").empty()
                ? Status::success()
                : Status{ErrorCode::OperationFailed,
                         "different Q shared Control dependencies"});
      auto selected =
          take(result.dependencies.restrict({{at ? "right" : "left", one}}));
      auto support = take(selected.source_support());
      check(support.at("control") == point
                ? Status::success()
                : Status{ErrorCode::OperationFailed,
                         "different Q shared captured ancestry"});
    }
    check(multi_result::number(result.results.at("left")) == 10 &&
                  multi_result::number(result.results.at("right")) == 11
              ? Status::success()
              : Status{ErrorCode::OperationFailed, "query values changed"});
  };
  auto cold = take(context.execute_fragments(frozen, wanted));
  verify(cold);
  PS_CHECK(*calls == 2);
  auto equivalent = take(context.freeze(
      plan,
      ExecutionBindings{{multi_result::binding(root, "control", 1, schema)}}));
  auto warm = take(context.execute_fragments(equivalent, wanted));
  verify(warm);
  PS_CHECK(*calls == 2 && warm.diagnostics.cache_hits >= 2 &&
           root.statistics().live[ResourceKind::Queue] == 0);
  auto builder = take(ResultBuilder::start(root, schema, "changed.control"));
  check(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(root, 1, {}))));
  const std::array<double, 2> values{1, -1};
  check(builder.publish_tensor(
      0, Region::whole({2}),
      {reinterpret_cast<const std::uint8_t*>(values.data()), sizeof(values)},
      take(ResultRelation::cartesian(root, 2, {})), {true, true, true, true}));
  ExecutionBinding binding;
  binding.name = "control";
  binding.result = take(builder.seal());
  auto changed = take(context.freeze(plan, ExecutionBindings{{binding}}));
  auto partial = take(context.execute_fragments(changed, wanted));
  PS_CHECK(multi_result::number(partial.results.at("left")) == 10 &&
           multi_result::number(partial.results.at("right")) == 21 &&
           *calls == 3 && partial.diagnostics.cache_hits >= 1 &&
           partial.diagnostics.joint_groups == 0);
  for (unsigned at = 0; at < 2; ++at) {
    auto point = take(Footprint::from_regions({2}, {Region({{at, 1}})}));
    auto dirty = take(partial.dependencies.potential_dirty(
        "control", point, 2, {}, ResultSupportTarget::Tensor, 0));
    PS_CHECK(dirty.at(at ? "right" : "left") == one &&
             dirty.at(at ? "left" : "right").empty());
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
      need.tensors.push_back(
          {0, 0, take(Footprint::from_regions({2}, {Region({{at, 1}})})), 1});
      return Result<ResultProgramPoll>(std::move(need));
    }
    double value = 0;
    check(phase.tensors->at({0, 0}).read({at}, &value, sizeof(value)));
    if (!builder) {
      builder.emplace(take(ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key)));
      check(builder->bind_descriptor_relation(
          take(ResultRelation::cartesian(phase.resources, 1, {}))));
    }
    const Region point({{at, 1}});
    auto relation = take(
        ResultRelation::mapped(phase.resources, {2}, point, {2}, {{0, 0, 1, 1}},
                               {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
    check(builder->publish_tensor(
        0, point,
        {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)},
        relation, {true, true, true, true}));
    return Result<ResultProgramPoll>(ResultPublication{
        stage == 4 ? take(builder->seal()) : builder->reference(), stage == 4});
  }
};
int partial_dependencies() {
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(controlled_definition(calls)));
  auto consumer = consumer_definition(false, 2);
  auto partial_schema = multi_result::schema(ElementType::Float64, {2});
  partial_schema.publication = PublishPolicy::StablePrefix;
  consumer.traits.outputs = {multi_result::output("value", partial_schema)};
  consumer.traits.outputs[0].maximum_dependency_stages = 4;
  consumer.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<Partial>(allocator);
  };
  check(registry->register_operation(std::move(consumer)));
  check(registry->freeze());
  WorkflowDocument doc;
  const auto schema = multi_result::schema(ElementType::Float64, {2});
  doc.inputs = {multi_result::declaration(1, "control", schema)};
  doc.nodes = {{10, "queries.source", {WorkflowInputReference{1}}, {}},
               {20, "queries.consumer", {WorkflowNodeOutput{10, "value"}}, {}}};
  doc.outputs = {{"value", 20, "value"}};
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 16, 1024 * 1024, 0});
  auto root = take(context.resource_budget());
  unsigned notices = 0;
  ResultRef first;
  ExecutionOptions options;
  options.result_publication = [&](ValueRef ref, const ResultRef& object) {
    if (ref.node_id == 20 && ++notices == 1)
      first = object;
    return Status::success();
  };
  auto frozen = take(context.freeze(
      plan,
      ExecutionBindings{{multi_result::binding(root, "control", 1, schema)}}));
  auto result = take(context.execute_fragments(
      frozen, {{"value", take(Footprint::all({2}))}}, {}, options));
  PS_CHECK(notices == 2 && *calls == 2 &&
           first.object_id() == result.results.at("value").object_id());
  PS_CHECK(first.descriptor(false).value().tensor_coverage(0) ==
           take(Footprint::from_regions({2}, {Region({{0, 1}})})));
  for (unsigned at = 0; at < 2; ++at) {
    const auto point = take(Footprint::from_regions({2}, {Region({{at, 1}})}));
    auto dirty = take(result.dependencies.potential_dirty(
        "control", point, 2, {}, ResultSupportTarget::Tensor, 0));
    PS_CHECK(dirty.at("value") == point);
    auto selected = take(result.dependencies.restrict({{"value", point}}));
    PS_CHECK(take(selected.source_support()).at("control") == point);
    PS_CHECK(multi_result::number(result.results.at("value"), {at}) == 10 + at);
  }
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
struct Mixed {
  unsigned stage = 0;
  double value = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (stage < 2) {
      if (stage == 1)
        check(phase.tensors->at({0, 0}).read({0}, &value, sizeof(value)));
      ResultProgramNeed need;
      need.tensors.push_back(
          {0, 0, take(Footprint::from_regions({2}, {Region({{stage, 1}})})),
           stage ? 9U : 1U});
      ++stage;
      return Result<ResultProgramPoll>(std::move(need));
    }
    return Result<ResultProgramPoll>(publication(
        phase, value, {0, 1, 0, 1, ResultSupportTarget::Tensor, 0}));
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
  const auto schema = multi_result::schema(ElementType::Float64, {2});
  doc.inputs = {multi_result::declaration(1, "control", schema)};
  doc.nodes = {{10, "queries.source", {WorkflowInputReference{1}}, {}},
               {20, "queries.consumer", {WorkflowNodeOutput{10, "value"}}, {}}};
  doc.outputs = {{"value", 20, "value"}};
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 16, 1024 * 1024, 1048576});
  auto root = take(context.resource_budget());
  const auto run = [&] {
    auto frozen =
        take(context.freeze(plan, ExecutionBindings{{multi_result::binding(
                                      root, "control", 1, schema)}}));
    return take(context.execute_fragments(
        frozen, {{"value", take(Footprint::all({1}))}}));
  };
  const auto verify = [&](const DemandResult& result) {
    auto selected =
        take(result.dependencies.restrict(result.dependencies.coverage()));
    auto support = take(selected.source_support());
    check(support.at("control") == take(Footprint::all({2}))
              ? Status::success()
              : Status{ErrorCode::OperationFailed,
                       "mixed target ancestry mismatch"});
    for (unsigned at = 0; at < 2; ++at) {
      auto point = take(Footprint::from_regions({2}, {Region({{at, 1}})}));
      auto dirty = take(selected.potential_dirty(
          "control", point, 2, {}, ResultSupportTarget::Tensor, 0));
      check(dirty.at("value") == take(Footprint::all({1}))
                ? Status::success()
                : Status{ErrorCode::OperationFailed,
                         "mixed target dirty mismatch"});
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
int upstream_peer_handoff() {
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
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {1, false, 16, 1024 * 1024, 0});
  auto root = take(context.resource_budget());
  auto frozen = take(context.freeze(plan));
  CancellationSource cancellation;
  auto first = std::async(std::launch::async, [&] {
    return context.execute(frozen, cancellation.token());
  });
  const bool entered = gate.wait();
  auto peer =
      std::async(std::launch::async, [&] { return context.execute(frozen); });
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (context.cache_statistics().shared_computations < 2 &&
         std::chrono::steady_clock::now() < until)
    std::this_thread::yield();
  const bool joined = context.cache_statistics().shared_computations >= 2;
  cancellation.cancel();
  const bool handed_off =
      first.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  gate.release();
  auto cancelled = first.get();
  auto healthy = peer.get();
  if (!entered || !joined || !handed_off || gate.timed_out)
    std::cerr << "upstream handoff entered=" << entered << " joined=" << joined
              << " handed_off=" << handed_off << " timed_out=" << gate.timed_out
              << '\n';
  PS_CHECK(entered && joined && handed_off && !gate.timed_out);
  PS_CHECK(cancelled.status().code == ErrorCode::Cancelled && healthy.ok());
  PS_CHECK(multi_result::number(healthy.value().results.at("left")) == 10 &&
           multi_result::number(healthy.value().results.at("right")) == 11);
  PS_CHECK(*calls == 2 && context.cache_statistics().in_flight == 0 &&
           root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
}  // namespace
int main() try {
  PS_CHECK(workflow(true, false) == 0);
  PS_CHECK(workflow(false, false) == 0);
  PS_CHECK(workflow(true, true) == 0);
  PS_CHECK(workflow(false, true) == 0);
  PS_CHECK(upstream_peer_handoff() == 0);
  PS_CHECK(query_dependencies(false) == 0);
  PS_CHECK(query_dependencies(true) == 0);
  PS_CHECK(partial_dependencies() == 0);
  PS_CHECK(mixed_target_dependencies() == 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
