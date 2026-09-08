#include <atomic>
#include <condition_variable>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
ps::Result<ps::Value> scalar(const ps::OperationInvocation& invocation,
                             double number) {
  auto made =
      ps::MutableValue::allocate({ps::ElementType::Float64, {1}},
                                 ps::Region::whole({1}), invocation.allocator);
  if (!made.ok())
    return ps::Result<ps::Value>(made.status());
  auto writer = made.take_value();
  std::memcpy(writer.data(), &number, sizeof(number));
  return std::move(writer).publish();
}
}  // namespace

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
  std::weak_ptr<const CpuStorage> source_storage;
  std::mutex mutex;
  std::condition_variable ready;
  bool entered = false, other_done = false;
  std::atomic<bool> lifetime_ok{true};
  OperationTraits source;
  PS_CHECK(
      registry
          ->register_operation({"source", source,
                                [&](const OperationInvocation& invocation) {
                                  auto result = scalar(invocation, 2);
                                  if (result.ok())
                                    source_storage = result.value().storage();
                                  return result;
                                }})
          .ok());
  OperationTraits unary;
  unary.input_count = 1;
  unary.input_schema.resize(1);
  unary.shape_rule = OperationShapeRule::PreserveFirstInput;
  unary.region_rule = OperationRegionRule::Elementwise;
  PS_CHECK(registry
               ->register_operation(
                   {"left", unary,
                    [&](const OperationInvocation& invocation) {
                      std::unique_lock<std::mutex> lock(mutex);
                      entered = true;
                      ready.notify_all();
                      ready.wait(lock, [&] { return other_done; });
                      lifetime_ok = !source_storage.expired();
                      lock.unlock();
                      return scalar(
                          invocation,
                          invocation.inputs[0].as_float64().value() + 1);
                    }})
               .ok());
  PS_CHECK(registry
               ->register_operation(
                   {"right", unary,
                    [&](const OperationInvocation& invocation) {
                      auto result =
                          scalar(invocation,
                                 invocation.inputs[0].as_float64().value() + 2);
                      std::unique_lock<std::mutex> lock(mutex);
                      ready.wait(lock, [&] { return entered; });
                      other_done = true;
                      ready.notify_all();
                      return result;
                    }})
               .ok());
  auto join = unary;
  join.input_count = 2;
  join.input_schema.resize(2);
  PS_CHECK(registry
               ->register_operation(
                   {"join", join,
                    [&](const OperationInvocation& invocation) {
                      lifetime_ok = lifetime_ok && source_storage.expired();
                      return scalar(
                          invocation,
                          invocation.inputs[0].as_float64().value() +
                              invocation.inputs[1].as_float64().value());
                    }})
               .ok());
  OperationTraits scratch_traits;
  scratch_traits.workspace_bytes = 32;
  PS_CHECK(registry
               ->register_operation({"scratch", scratch_traits,
                                     [](const OperationInvocation& invocation) {
                                       auto scratch =
                                           invocation.allocator.allocate(32);
                                       if (!scratch.ok())
                                         return Result<Value>(scratch.status());
                                       return scalar(invocation, 5);
                                     }})
               .ok());
  PS_CHECK(registry
               ->register_operation({"excess", scratch_traits,
                                     [](const OperationInvocation& invocation) {
                                       auto scratch =
                                           invocation.allocator.allocate(33);
                                       if (!scratch.ok())
                                         return Result<Value>(scratch.status());
                                       return scalar(invocation, 5);
                                     }})
               .ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.nodes = {
      {1, "source", {}, {}},
      {2, "left", {WorkflowNodeOutput{1, "value"}}, {}},
      {3, "right", {WorkflowNodeOutput{1, "value"}}, {}},
      {4,
       "join",
       {WorkflowNodeOutput{2, "value"}, WorkflowNodeOutput{3, "value"}},
       {}}};
  document.outputs = {{"result", 4, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  Value retained;
  {
    ExecutionContext execution(registry, {2, false, 8, 32});
    auto result = execution.execute(compiled.value().plan);
    PS_CHECK(result.ok() && lifetime_ok);
    PS_CHECK(result.value().diagnostics.peak_live_bytes == 24);
    PS_CHECK(result.value().diagnostics.planned_peak_bytes == 32);
    PS_CHECK(result.value().diagnostics.peak_active_tasks <= 2);
    retained = result.value().values.at("result");
    PS_CHECK(retained.as_float64().value() == 7);
    auto held = execution.execute(compiled.value().plan);
    PS_CHECK(!held.ok() && held.status().code == ErrorCode::ResourceExhausted);
    retained = Value();
    result = Result<ExecutionResult>(ExecutionResult{});
    auto recovered = execution.execute(compiled.value().plan);
    PS_CHECK(recovered.ok());
    retained = recovered.value().values.at("result");
  }
  PS_CHECK(retained.as_float64().value() == 7 &&
           retained.storage()->accounted());
  ExecutionContext short_budget(registry, {2, false, 8, 31});
  PS_CHECK(short_budget.execute(compiled.value().plan).status().code ==
           ErrorCode::ResourceExhausted);
  for (const auto& key : {"scratch", "excess"}) {
    WorkflowDocument probe;
    probe.nodes = {{1, key, {}, {}}};
    probe.outputs = {{"result", 1, "value"}};
    GraphContext probe_graph(probe);
    auto plan = compiler.compile(probe_graph);
    PS_CHECK(plan.ok());
    ExecutionContext exact(registry, {1, false, 4, 40});
    auto result = exact.execute(plan.value().plan);
    if (std::string(key) == "scratch") {
      PS_CHECK(result.ok() && result.value().diagnostics.peak_live_bytes == 40);
    } else {
      PS_CHECK(!result.ok() &&
               result.status().code == ErrorCode::ResourceExhausted);
    }
    ExecutionContext insufficient(registry, {1, false, 4, 39});
    PS_CHECK(insufficient.execute(plan.value().plan).status().code ==
             ErrorCode::ResourceExhausted);
  }
  return 0;
}
