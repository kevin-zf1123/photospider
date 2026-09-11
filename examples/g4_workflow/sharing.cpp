#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "photospider/photospider.hpp"

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
}  // namespace
void sharing_workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto gate = std::make_shared<Gate>();
  auto operations = std::make_shared<OperationRegistry>();
  OperationDefinition identity;
  identity.key = "example.shared_identity";
  identity.traits.input_count = 1;
  identity.traits.input_schema.resize(1);
  identity.traits.outputs[0].shape_rule =
      OperationShapeRule::PreserveFirstInput;
  identity.traits.outputs[0].region_rule = OperationRegionRule::Elementwise;
  identity.callback = [gate](const OperationInvocation& call) -> Result<Value> {
    std::unique_lock<std::mutex> lock(gate->mutex);
    ++gate->calls;
    gate->changed.notify_all();
    if (!gate->changed.wait_for(lock, std::chrono::seconds(3),
                                [&] { return gate->release; }))
      return Result<Value>(
          Status{ErrorCode::OperationFailed, "example gate deadline"});
    if (call.cancellation.cancelled())
      return Result<Value>(Status{ErrorCode::Cancelled, {}});
    return Result<Value>(call.inputs[0]);
  };
  require(operations->register_operation(std::move(identity)).ok(),
          "register shared identity");
  require(operations->freeze().ok(), "freeze shared registry");
  WorkflowDocument document;
  document.inputs = {
      {1, "x", {ElementType::Float64, {1}}, Region::whole({1}), {0, {8}}, {}}};
  document.nodes = {
      {1, "example.shared_identity", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"y", 1, "value"}};
  GraphContext graph(document);
  auto compiled = Compiler(operations).compile(graph);
  require(compiled.ok(), "compile sharing workflow");
  ExecutionContext context(operations, {1, false, 8, 4096});
  auto writer = MutableValue::allocate({ElementType::Float64, {1}},
                                       Region::whole({1}), BufferAllocator{})
                    .take_value();
  const double expected = 7;
  std::memcpy(writer.data(), &expected, 8);
  auto opened =
      context.open_demand(compiled.value().plan,
                          {{{"x", std::move(writer).publish().take_value()}}});
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
  double actual = 0;
  require(survivor.value().values.at("y").read({0}, &actual, 8).ok() &&
              actual == expected,
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
