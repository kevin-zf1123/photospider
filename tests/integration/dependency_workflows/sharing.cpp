#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "support/dependency_workflow_fixture.hpp"

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned calls = 0;
  bool release = false;
};
struct SharedIdentity {
  std::shared_ptr<Gate> gate;
  bool requested = false;
  explicit SharedIdentity(std::shared_ptr<Gate> gate) : gate(std::move(gate)) {}
  ps::Result<ps::ResultProgramPoll> poll(
      const ps::ResultProgramPhase& phase) try {
    using namespace ps;  // NOLINT(build/namespaces)
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tensors.push_back(
          {0, 0, dependency_fixture::take(Footprint::all({1})), 9});
      return Result<ResultProgramPoll>(std::move(need));
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    ++gate->calls;
    gate->changed.notify_all();
    if (!gate->changed.wait_for(lock, std::chrono::seconds(3),
                                [&] { return gate->release; }))
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed, "example gate deadline"});
    if (phase.query.cancellation.cancelled())
      return Result<ResultProgramPoll>(Status{ErrorCode::Cancelled, {}});
    lock.unlock();
    auto builder = dependency_fixture::take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{},
        phase.query.tile_height, phase.query.tile_width,
        phase.query.resources));
    dependency_fixture::check(builder.bind_descriptor_relation(
        dependency_fixture::take(ResultRelation::cartesian(
            phase.resources, 1,
            {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0}))));
    const auto box = Region::whole({1});
    auto window =
        dependency_fixture::take(phase.tensors->at({0, 0}).acquire(box));
    ResultTensorViewTransform identity;
    identity.source_axes = {{0, 0, 1, 1}};
    auto relation = dependency_fixture::take(ResultRelation::mapped(
        phase.resources, {1}, box, {1}, identity.source_axes,
        {0, 1, 0, 1, ResultSupportTarget::Tensor, 0}));
    dependency_fixture::check(builder.publish_tensor_view(
        0, box, window, identity, std::move(relation),
        {true, true, true, true}));
    return Result<ResultProgramPoll>(
        ResultPublication{dependency_fixture::take(builder.seal()), true});
  } catch (const dependency_fixture::Failure& failed) {
    return ps::Result<ps::ResultProgramPoll>(failed.status);
  }
};
}  // namespace
void sharing_workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto gate = std::make_shared<Gate>();
  auto operations = std::make_shared<OperationRegistry>();
  OperationDefinition identity;
  identity.key = "example.shared_identity";
  identity.traits.input_count = 1;
  identity.traits.input_schema.resize(1);
  const auto schema = dependency_fixture::schema(ElementType::Float64, 1);
  identity.traits.input_schema[0].kind = OperationPortKind::Result;
  identity.traits.input_schema[0].result_schema_id = std::string(schema.id);
  identity.traits.input_schema[0].result_schema_version = schema.version;
  identity.traits.outputs = {
      dependency_fixture::output(schema, sizeof(SharedIdentity))};
  identity.start_result = [gate](const ResultProgramQuery&,
                                 const BufferAllocator& allocator) {
    return ResultContinuation::make<SharedIdentity>(allocator, gate);
  };
  require(operations->register_operation(std::move(identity)).ok(),
          "register shared identity");
  require(operations->freeze().ok(), "freeze shared registry");
  WorkflowDocument document;
  document.inputs = {dependency_fixture::declaration(1, "x", schema)};
  document.nodes = {
      {1, "example.shared_identity", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"y", 1, "value"}};
  GraphContext graph(document);
  auto compiled = Compiler(operations).compile(graph);
  require(compiled.ok(), "compile sharing workflow");
  ExecutionContext context(operations, {1, false, 8, 4096});
  const double expected = 7;
  ExecutionBinding binding;
  binding.name = "x";
  binding.result = dependency_fixture::numbers(
      dependency_fixture::take(context.resource_budget()), {expected});
  auto opened = context.open_demand(compiled.value().plan, {{binding}});
  require(opened.ok(), "open sharing demand");
  auto demand = opened.take_value();
  const DemandQuery q{{"y", Footprint::all({1}).take_value()}};
  CancellationSource cancelled;
  auto first = std::async(std::launch::async,
                          [&] { return demand.request(q, cancelled.token()); });
  {
    std::unique_lock<std::mutex> lock(gate->mutex);
    require(gate->changed.wait_for(lock, std::chrono::seconds(3),
                                   [&] { return gate->calls == 1; }),
            "first callback did not start");
  }
  auto second =
      std::async(std::launch::async, [&] { return demand.request(q); });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!context.cache_statistics().shared_computations) {
    require(std::chrono::steady_clock::now() < deadline,
            "second waiter did not join");
    std::this_thread::yield();
  }
  cancelled.cancel();
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->release = true;
    gate->changed.notify_all();
  }
  require(first.get().status().code == ErrorCode::Cancelled,
          "first waiter cancellation");
  auto survivor = second.get();
  require(survivor.ok(), "surviving waiter failed");
  require(
      dependency_fixture::number(survivor.value().results.at("y")) == expected,
      "shared identity oracle");
  require(
      gate->calls == 1 && survivor.value().diagnostics.shared_computations == 1,
      "duplicate computation");
  require(survivor.value()
                  .dependencies.potential_dirty("x", q.at("y"))
                  .value()
                  .at("y") == q.at("y"),
          "shared evidence oracle");
  std::cout
      << "shared: callbacks=1, first=Cancelled, second=7, evidence=present\n";
}
