#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <vector>

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
  doc.inputs = {
      {1, "x", {ElementType::Float64, {2}}, Region::whole({2}), {0, {8}}, {}}};
  doc.nodes = {{1,
                "numeric.ordered_scan",
                {WorkflowInputReference{1}},
                {{"block_size", INT64_C(1)}}}};
  doc.outputs = {{"y", 1, "value"}};
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {2, false, 8, 4096, warm ? 64U : 0U});
  auto demand =
      context.open_demand(plan, {{{"x", input(finite)}}}).take_value();
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
               result.value().values.at("y").read({i}, &actual, 8).ok() &&
               actual == (i ? 3 : 1));
      auto support = result.value().dependencies.source_support();
      PS_CHECK(support.ok());
      auto expected = Footprint::from_regions({2}, {Region({{0, i + 1}})});
      PS_CHECK(support.value().at("x") == expected.value());
    }
  }
  return 0;
}
}  // namespace
int main() {
  for (bool long_first : {false, true}) {
    PS_CHECK(shared(long_first, false, false, false) == 0);
    PS_CHECK(shared(long_first, true, false, false) == 0);
    PS_CHECK(shared(long_first, true, true, false) == 0);
  }
  PS_CHECK(shared(true, false, false, true) == 0);
  PS_CHECK(shared(true, true, false, true) == 0);
  return 0;
}
