#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include <memory>

#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
int validator_library_lifetime() {
#if !defined(_WIN32)
  using namespace ps;  // NOLINT(build/namespaces)
  using multi_result::check;
  using multi_result::take;
  struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned entered = 0;
    bool release = false;
    std::atomic<unsigned> states{0}, modules{0};
  } gate;
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->load_plugin(PS_EXTENSIONS_FIXTURE));
  check(registry->freeze());
  auto* observer = dlopen(PS_EXTENSIONS_FIXTURE, RTLD_NOW | RTLD_LOCAL);
  PS_CHECK(observer);
  auto setter = reinterpret_cast<void (*)(void (*)(void*, uint32_t), void*)>(
      dlsym(observer, "ps_extension_set_lifetime_event"));
  PS_CHECK(setter);
  setter(
      [](void* raw, uint32_t event) {
        auto& gate = *static_cast<Gate*>(raw);
        if (event == 1) {
          ++gate.states;
          return;
        }
        if (event == 2) {
          ++gate.modules;
          return;
        }
        std::unique_lock<std::mutex> lock(gate.mutex);
        ++gate.entered;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&] { return gate.release; });
      },
      &gate);
  dlclose(observer);
  ResourceBudget root;
  const std::uint8_t abc[] = {'a', 'b', 'c'};
  auto resource = take(ImmutableResource::import({abc, 3}, root));
  auto resources = take(ResourceSet::create({resource}, root));
  auto prepared = take(
      registry->prepare_operation("fixture.c_extensions", {}, {}, resources));
  auto schema = *prepared->traits().outputs[0].result_schema;
  auto builder = take(ResultBuilder::start(root, schema, "validator"));
  check(builder.publish_tensor(0, Region::whole({3}), {abc, 3}));
  auto result = take(builder.seal());
  auto descriptor = take(result.descriptor());
  auto coverage = descriptor.tensor_coverage(0);
  CancellationToken token;
  ValidationContext context{schema, root, token};
  context.complete = true;
  context.consume = [&](auto n) { return root.consume({n}); };
  context.tensor_coverage = [&](uint32_t slot) {
    return slot == 0 ? &coverage : nullptr;
  };
  context.acquire_tensor = [&](uint32_t slot, const Region& r) {
    return result.acquire_tensor(descriptor, slot, r);
  };
  auto a = std::async(std::launch::async, [prepared, &context] {
    return prepared->validate_port(true, 0, context);
  });
  auto b = std::async(std::launch::async, [prepared, &context] {
    return prepared->validate_port(true, 0, context);
  });
  bool entered;
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    entered = gate.changed.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return gate.entered == 2; });
  }
  registry.reset();
  prepared.reset();
  const bool retained = gate.states == 0 && gate.modules == 0;
  {
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.release = true;
  }
  gate.changed.notify_all();
  auto first = a.get(), second = b.get();
  PS_CHECK(entered && retained);
  PS_REQUIRE_OK(first);
  PS_REQUIRE_OK(second);
  PS_CHECK(gate.states == 1 && gate.modules == 1);
#endif
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(validator_library_lifetime() == 0);
  using namespace ps;  // NOLINT(build/namespaces)
  using multi_result::check;
  using multi_result::take;
  for (const auto* path : {PS_EXTENSION_FAILURE_1, PS_EXTENSION_FAILURE_2,
                           PS_EXTENSION_FAILURE_6}) {
    OperationRegistry rejected;
    PS_CHECK(rejected.load_plugin(path).code == ErrorCode::InvalidArgument);
    PS_CHECK(rejected.keys().empty());
    check(rejected.load_plugin(PS_EXTENSIONS_FIXTURE));
    check(rejected.freeze());
  }
  for (const auto* path : {PS_EXTENSION_FAILURE_3, PS_EXTENSION_FAILURE_4}) {
    OperationRegistry failing;
    check(failing.load_plugin(path));
    check(failing.freeze());
    auto prepared = failing.prepare_operation("fixture.c_extensions", {}, {});
    PS_CHECK(!prepared.ok());
    PS_CHECK(prepared.status().code ==
             (std::strcmp(path, PS_EXTENSION_FAILURE_3) == 0
                  ? ErrorCode::InvalidArgument
                  : ErrorCode::OperationFailed));
    if (std::strcmp(path, PS_EXTENSION_FAILURE_4) == 0)
      PS_CHECK(prepared.status().reason == FailureReason::HostException);
  }
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->load_plugin(PS_EXTENSIONS_FIXTURE));
  check(registry->freeze());
  ResourceBudget root;
  const std::uint8_t abc[] = {'a', 'b', 'c'};
  auto resource = take(ImmutableResource::import({abc, 3}, root));
  auto resources = take(ResourceSet::create({resource}, root));
  WorkflowDocument document;
  document.nodes = {{1, "fixture.c_extensions", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto missing = Compiler(registry).compile(graph);
  PS_CHECK(!missing.ok() && missing.status().code == ErrorCode::NotFound);
  auto compiled = take(Compiler(registry).compile(graph, {}, resources));
  ExecutionContextConfig config;
  config.result_cache_bytes = 1048576;
  ExecutionContext context(registry, config);
  auto execution = take(context.execute(compiled.plan));
  const auto& result = execution.results.at("value");
  const auto facts = take(result.descriptor());
  for (std::uint64_t i = 0; i < 3; ++i) {
    std::uint8_t byte = 0;
    check(result.read_tensor(facts, 0, {i}, &byte, 1));
    PS_CHECK(byte == abc[i]);
  }
  const auto& report = execution.diagnostics.operation_timings[0].reports;
  PS_CHECK(report.has_value());
  auto bytes = take(report->find({"fixture.count", 1, "fixture/1"}));
  std::uint64_t count = 0;
  std::memcpy(&count, bytes.data(), 8);
  PS_CHECK(count == 3);
  auto cached = take(context.execute(compiled.plan));
  PS_CHECK(cached.diagnostics.cache_hits > 0 &&
           cached.diagnostics.operation_timings.empty());
  auto failing = std::make_shared<OperationRegistry>();
  check(failing->load_plugin(PS_EXTENSION_FAILURE_5));
  check(failing->freeze());
  auto invalid_plan = take(Compiler(failing).compile(graph, {}, resources));
  ExecutionContext invalid_context(failing);
  auto invalid = invalid_context.execute(invalid_plan.plan);
  PS_CHECK(!invalid.ok() &&
           invalid.status().reason != FailureReason::HostException);
  auto domain_registry = std::make_shared<OperationRegistry>();
  check(domain_registry->load_plugin(PS_EXTENSION_FAILURE_7));
  check(domain_registry->freeze());
  auto domain_plan =
      take(Compiler(domain_registry).compile(graph, {}, resources));
  ExecutionContext domain_context(domain_registry);
  auto domain_failure = domain_context.execute(domain_plan.plan);
  PS_CHECK(!domain_failure.ok() && domain_failure.status().detail.domain_code ==
                                       UINT64_C(0x123456789abcdef0));
  return 0;
}
