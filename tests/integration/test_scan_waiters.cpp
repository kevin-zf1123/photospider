#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "execution/execution_test_hooks.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;                    // NOLINT(build/namespaces)
using namespace std::chrono_literals;  // NOLINT(build/namespaces)
struct Gate final {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, released = false;
  std::atomic<unsigned> borrowed{0};
  void hold() {
    std::unique_lock<std::mutex> lock(mutex);
    if (entered)
      return;
    entered = true;
    changed.notify_all();
    changed.wait_for(lock, 5s, [&] { return released; });
  }
  bool wait() {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, 3s, [&] { return entered; });
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
};
Gate* active = nullptr;
void hold_checkpoint() noexcept {
  active->hold();
}
void borrow_checkpoint() noexcept {
  ++active->borrowed;
}
Value input(bool finite) {
  const double data[]{1, finite ? 2 : std::numeric_limits<double>::infinity()};
  std::vector<std::uint8_t> bytes(sizeof(data));
  std::memcpy(bytes.data(), data, sizeof(data));
  return Value::create({ElementType::Float64, {2}}, Region::whole({2}),
                       {0, {8}}, bytes)
      .take_value();
}
Footprint point(unsigned index) {
  return Footprint::from_regions({2}, {Region({{index, 1}})}).take_value();
}
int shared(bool long_first, bool cancel_owner, bool finite, bool warm) {
  auto registry = make_default_operation_registry();
  WorkflowDocument doc;
  WorkflowInputDeclaration declaration;
  declaration.id = 1;
  declaration.name = "x";
  declaration.result_schema = std::make_shared<SchemaTemplate>(
      numeric_result_fixture::source_schema(input(finite)));
  doc.inputs = {declaration};
  doc.nodes = {{1,
                "numeric.ordered_scan",
                {WorkflowInputReference{1}},
                {{"block_size", INT64_C(1)}}}};
  doc.outputs = {{"y", 1, "value"}};
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.result_cache_bytes = warm ? 64 : 0;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = context.resource_budget().take_value();
  auto source = numeric_result_fixture::source(root, input(finite));
  auto demand = context.open_demand(plan, {{{"x", source}}}).take_value();
  if (warm)
    PS_CHECK(demand.request({{"y", point(0)}}).ok());
  Gate gate;
  active = &gate;
  execution_testing::ExecutionTestHooks hooks;
  hooks.checkpoint_published = hold_checkpoint;
  hooks.checkpoint_borrowed = borrow_checkpoint;
  execution_testing::install_execution_test_hooks(&hooks);
  CancellationSource stop;
  const auto owner_index = long_first ? 1U : 0U;
  const auto other_index = 1U - owner_index;
  auto owner = std::async(std::launch::async, [&] {
    return demand.request({{"y", point(owner_index)}}, stop.token());
  });
  const bool entered = gate.wait();
  // Both futures are always drained before the borrowed global hooks retire.
  auto other = std::async(std::launch::async, [&] {
    return demand.request({{"y", point(other_index)}});
  });
  const bool independent = other.wait_for(3s) == std::future_status::ready;
  if (cancel_owner)
    stop.cancel();
  gate.release();
  auto first = owner.get();
  auto second = other.get();
  execution_testing::install_execution_test_hooks(nullptr);
  active = nullptr;
  PS_CHECK(entered && independent);
  PS_CHECK(warm || gate.borrowed.load() > 0);
  for (unsigned i = 0; i < 2; ++i) {
    const auto& result = i == owner_index ? first : second;
    if (cancel_owner && i == owner_index) {
      PS_CHECK(result.status().code == ErrorCode::Cancelled);
    } else if (i == 1 && !finite) {
      PS_CHECK(result.status().code == ErrorCode::OperationFailed &&
               result.status().message == "nonfinite scan input 1");
    } else {
      double actual = 0;
      PS_CHECK(result.ok() &&
               result.value()
                   .results.at("y")
                   .read_tensor(
                       result.value().results.at("y").descriptor().take_value(),
                       0, {i}, &actual, 8)
                   .ok() &&
               actual == (i ? 3 : 1));
      auto support = result.value().dependencies.source_support();
      PS_CHECK(support.ok());
      auto expected = Footprint::from_regions({2}, {Region({{0, i + 1}})});
      PS_CHECK(support.value().at("x") == expected.value());
    }
  }
  return 0;
}
int warm_short_first(bool cancel_short, bool finite, bool retain_result) {
  auto registry = make_default_operation_registry();
  WorkflowDocument doc;
  WorkflowInputDeclaration declaration;
  declaration.id = 1;
  declaration.name = "x";
  declaration.result_schema = std::make_shared<SchemaTemplate>(
      numeric_result_fixture::source_schema(input(finite)));
  doc.inputs = {declaration};
  doc.nodes = {{1,
                "numeric.ordered_scan",
                {WorkflowInputReference{1}},
                {{"block_size", INT64_C(1)}}}};
  doc.outputs = {{"y", 1, "value"}};
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.result_cache_bytes = 64;
  config.maximum_dependency_cache_metadata = retain_result ? 65536 : 1;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = context.resource_budget().take_value();
  auto source = numeric_result_fixture::source(root, input(finite));
  auto demand = context.open_demand(plan, {{{"x", source}}}).take_value();
  PS_CHECK(demand.request({{"y", point(0)}}).ok());
  CancellationSource stop;
  if (cancel_short)
    stop.cancel();
  // A completed content block may be reused after the previous Run retires.
  // Submit the short observation first and preserve its own outcome.
  auto short_result = demand.request({{"y", point(0)}}, stop.token());
  auto long_result = demand.request({{"y", point(1)}});
  if (cancel_short) {
    PS_CHECK(short_result.status().code == ErrorCode::Cancelled);
  } else {
    double value = 0;
    PS_CHECK(short_result.ok());
    const auto& diagnostics = short_result.value().diagnostics;
    if (retain_result) {
      PS_CHECK(diagnostics.shared_computations == 1 &&
               diagnostics.operation_timings.empty());
    } else {
      PS_CHECK(diagnostics.block_cache_hits == 1 &&
               diagnostics.block_cache_misses == 0);
    }
    PS_CHECK(
        short_result.value()
            .results.at("y")
            .read_tensor(
                short_result.value().results.at("y").descriptor().take_value(),
                0, {0}, &value, 8)
            .ok() &&
        value == 1);
  }
  if (finite) {
    double value = 0;
    PS_CHECK(
        long_result.ok() &&
        long_result.value()
            .results.at("y")
            .read_tensor(
                long_result.value().results.at("y").descriptor().take_value(),
                0, {1}, &value, 8)
            .ok() &&
        value == 3);
  } else {
    PS_CHECK(long_result.status().code == ErrorCode::OperationFailed &&
             long_result.status().message == "nonfinite scan input 1");
  }
  PS_CHECK(demand.request({{"y", point(0)}}).ok());
  return 0;
}
}  // namespace
int main() {
  for (bool long_first : {false, true}) {
    PS_CHECK(shared(long_first, false, false, false) == 0);
    PS_CHECK(shared(long_first, true, false, false) == 0);
    PS_CHECK(shared(long_first, true, true, false) == 0);
  }
  for (bool cancel : {false, true})
    for (bool finite : {false, true})
      for (bool retain_result : {false, true})
        PS_CHECK(warm_short_first(cancel, finite, retain_result) == 0);
  PS_CHECK(shared(true, false, false, true) == 0);
  PS_CHECK(shared(true, true, false, true) == 0);
  return 0;
}
