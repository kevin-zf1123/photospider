#include <dlfcn.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "photospider/plugin/result_operation_plugin_api.h"
#include "plugin/library_test_hooks.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
int loading() {
  const std::array<const char*, 6> invalid = {
      PS_ABI_VERSION,        PS_ABI_SIZE,           PS_ABI_OPERATION,
      PS_ABI_UNSUPPORTED_V2, PS_ABI_UNSUPPORTED_V1, PS_ABI_FAILURE_DELIVERY};
  for (std::size_t i = 0; i < invalid.size(); ++i) {
    OperationRegistry registry;
    auto status = registry.load_plugin(invalid[i]);
    PS_CHECK(status.code == ((i == 3 || i == 4) ? ErrorCode::TypeMismatch
                                                : ErrorCode::InvalidArgument));
    // Failed imports must leave the registry usable and unmodified.
    PS_REQUIRE_OK(registry.load_plugin(PS_ABI_VALID));
    PS_REQUIRE_OK(registry.freeze());
  }
  return 0;
}
std::atomic<unsigned> native_closes{0};
void closed(plugin_testing::LibraryKind) noexcept {
  ++native_closes;
}
struct LifetimeGate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, released = false, timed_out = false;
  std::atomic<unsigned> states{0}, modules{0};
  static void event(void* raw, std::uint32_t kind) {
    auto& gate = *static_cast<LifetimeGate*>(raw);
    if (kind == 1) {
      ++gate.states;
    } else if (kind == 2) {
      ++gate.modules;
    } else {
      std::unique_lock<std::mutex> lock(gate.mutex);
      gate.entered = true;
      gate.changed.notify_all();
      gate.timed_out = !gate.changed.wait_for(lock, std::chrono::seconds(10),
                                              [&] { return gate.released; });
    }
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
};
int active_module_owner() {
  LifetimeGate gate;
  native_closes = 0;
  plugin_testing::LibraryTestHooks hooks;
  hooks.native_close = closed;
  plugin_testing::install_library_test_hooks(&hooks);
  struct Unhook {
    ~Unhook() { plugin_testing::install_library_test_hooks(nullptr); }
  } unhook;
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->load_plugin(PS_ABI_VALID));
  check(registry->freeze());
  // Borrow the setter only long enough to install a host-owned gate. Drop this
  // extra dlopen reference before the callback starts, so it cannot mask
  // unload.
  void* observer = dlopen(PS_ABI_VALID, RTLD_NOW | RTLD_LOCAL);
  PS_CHECK(observer);
  auto set_event =
      reinterpret_cast<void (*)(void (*)(void*, std::uint32_t), void*)>(
          dlsym(observer, "ps_result_abi_set_lifetime_event"));
  PS_CHECK(set_event);
  set_event(LifetimeGate::event, &gate);
  dlclose(observer);
  WorkflowDocument document;
  document.nodes = {{1, "test.c_buffer", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  auto context = std::make_unique<ExecutionContext>(registry);
  auto frozen = take(context->freeze(plan));
  auto metadata = *plan.steps()[0].structured_metadata;
  const std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query(metadata, parameters);
  query.semantic_key = "active-module";
  ResourceBudget root;
  auto allocator = root.allocator();
  auto continuation =
      take(registry->start_result("test.c_buffer", query, allocator));
  ResultObjectInputs objects;
  ResultProgramPhase phase{query,
                           objects,
                           allocator,
                           root,
                           [&](std::uint64_t n) { return root.consume({n}); },
                           {}};
  auto pending =
      std::async(std::launch::async, [&] { return continuation.poll(phase); });
  struct Release {
    LifetimeGate& gate;
    ~Release() { gate.release(); }
  } release{gate};
  bool entered;
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    entered = gate.changed.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return gate.entered; });
  }
  frozen = {};
  plan = {};
  context.reset();
  registry.reset();
  PS_CHECK(entered && gate.states == 0 && gate.modules == 0 &&
           native_closes == 0);
  gate.release();
  auto answer = pending.get();
  PS_REQUIRE_OK(answer);
  PS_CHECK(!gate.timed_out && gate.states == 0 && gate.modules == 0 &&
           native_closes == 0);
  auto retained = std::get<ResultPublication>(answer.take_value()).result;
  continuation = {};
  PS_CHECK(gate.states == 1 && gate.modules == 1 && native_closes == 1);
  float value = 0;
  check(retained.read_tensor(take(retained.descriptor()), 0, {1}, &value,
                             sizeof(value)));
  PS_CHECK(value == 7);
  retained = {};
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int publication_owners() {
  ResultRef retained;
  ResourceBudget observed;
  {
    auto registry = std::make_shared<OperationRegistry>();
    check(registry->load_plugin(PS_ABI_VALID));
    check(registry->freeze());
    WorkflowDocument document;
    document.nodes = {
        {1, "test.c_buffer", {}, {}},
        {2, "test.c_reverse_view", {WorkflowNodeOutput{1, "value"}}, {}}};
    document.outputs = {{"view", 2, "value"}};
    GraphContext graph(document);
    auto plan = take(Compiler(registry).compile(graph)).plan;
    ExecutionContext context(registry);
    observed = take(context.resource_budget());
    auto answer = take(context.execute(plan));
    retained = answer.results.at("view");
    auto record = take(answer.dependencies.tile_record({1, 0, 0}));
    PS_CHECK(record && record->inputs.size() == 1 && record->inputs[0].whole);
  }
  // The view retains transferred scratch after the C windows, continuation,
  // context and registry (including the loaded library) have all retired.
  PS_CHECK(observed.statistics().live[ResourceKind::Payload] == 8);
  {
    const auto facts = take(retained.descriptor());
    PS_CHECK(facts.tensor_coverage(0) == take(Footprint::all({2})));
    for (std::uint64_t x = 0; x < 2; ++x) {
      float value = 0;
      check(retained.read_tensor(facts, 0, {x}, &value, 4));
      PS_CHECK(value == (x ? 3 : 7));
    }
  }
  retained = {};
  for (auto live : observed.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
}  // namespace
int main() {
  return loading() || active_module_owner() || publication_owners();
}
