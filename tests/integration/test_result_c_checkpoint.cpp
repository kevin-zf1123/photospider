#include <dlfcn.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using numeric_result_fixture::take;
struct Module {
  void* handle = dlopen(PS_RESULT_CHECKPOINT_FIXTURE, RTLD_NOW | RTLD_LOCAL);
  ~Module() {
    if (handle)
      dlclose(handle);
  }
  std::array<std::uint64_t, 6> counts() const {
    auto function = reinterpret_cast<void (*)(std::uint64_t*)>(
        dlsym(handle, "ps_result_checkpoint_counts"));
    if (!function)
      throw std::runtime_error("missing C checkpoint counts");
    std::array<std::uint64_t, 6> result{};
    function(result.data());
    return result;
  }
  unsigned gate(unsigned action) const {
    auto function = reinterpret_cast<unsigned (*)(unsigned)>(
        dlsym(handle, "ps_result_checkpoint_gate"));
    if (!function)
      throw std::runtime_error("missing C checkpoint gate");
    return function(action);
  }
};
SchemaTemplate schema() {
  SchemaTemplate result;
  result.id = "fixture.checkpoint.input";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ElementType::Float64, {5}};
  result.tensors.push_back(std::move(tensor));
  return result;
}
WorkflowDocument document(unsigned mode, const std::string& key) {
  WorkflowDocument result;
  WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "source";
  input.result_schema = std::make_shared<const SchemaTemplate>(schema());
  result.inputs = {input};
  result.nodes = {{10,
                   key,
                   {WorkflowInputReference{1}},
                   {{"mode", static_cast<std::int64_t>(mode)}}}};
  result.outputs = {{"value", 10, "value"}};
  return result;
}
ExecutionBindings binding(const ResourceBudget& root, double first = 1,
                          double second = 2) {
  const std::array<double, 5> numbers{first, second, 3, 4, 5};
  std::vector<std::uint8_t> bytes(sizeof(numbers));
  std::memcpy(bytes.data(), numbers.data(), bytes.size());
  auto value = take(Value::create({ElementType::Float64, {5}},
                                  Region::whole({5}), {0, {8}}, bytes));
  auto metadata = schema();
  return {{{"source", numeric_result_fixture::source(root, value, &metadata)}}};
}
Footprint point(std::uint64_t index) {
  return take(Footprint::from_regions({5}, {Region({{index, 1}})}));
}
double number(const ResultRef& result, std::uint64_t at) {
  double value = 0;
  auto status = numeric_result_fixture::read(result, {at}, &value, 8);
  if (!status.ok())
    throw std::runtime_error(status.message);
  return value;
}
int workflow(unsigned mode, bool cache = true,
             const std::string& key = "fixture.result_checkpoint") {
  Module module;
  PS_CHECK(module.handle);
  auto registry = std::make_shared<OperationRegistry>();
  auto loaded = registry->load_plugin(PS_RESULT_CHECKPOINT_FIXTURE);
  if (!loaded.ok())
    std::cerr << loaded.message << '\n';
  PS_CHECK(loaded.ok() && registry->freeze().ok());
  GraphContext graph(document(mode, key));
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = cache ? 1048576 : 0;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  auto demand = take(context.open_demand(plan, binding(root)));
  ExecutionOptions options;
  if (!cache)
    options.maximum_dependency_cache_work = 0;
  auto first = demand.request({{"value", point(2)}}, {}, options);
  if (mode == 0 || mode == 12) {
    if (!first.ok())
      std::cerr << "checkpoint mode " << mode << ": " << first.status().message
                << " code=" << static_cast<int>(first.status().code)
                << " reason=" << static_cast<int>(first.status().reason)
                << " origin=" << static_cast<int>(first.status().detail.origin)
                << '\n';
    if (!first.ok()) {
      for (auto count : module.counts())
        std::cerr << count << ' ';
      std::cerr << '\n';
    }
    PS_CHECK(first.ok() && number(first.value().results.at("value"), 2) == 6);
    auto before = module.counts();
    PS_CHECK(before[0] == 1 && before[1] == 1 && before[2] == 3 &&
             before[3] == 0 && before[4] == 3 && before[5] == (cache ? 3 : 0));
    auto second = demand.request({{"value", point(4)}}, {}, options);
    auto after = module.counts();
    {
      if (!second.ok())
        std::cerr << second.status().message << '\n';
      PS_CHECK(second.ok() &&
               number(second.value().results.at("value"), 4) == 15 &&
               after[0] == 2 && after[1] == 2 && after[2] - before[2] == 5 &&
               after[3] == 0);
      auto dirty =
          take(second.value().dependencies.potential_dirty("source", point(0)));
      PS_CHECK(dirty.at("value") == point(4));
      auto irrelevant =
          take(second.value().dependencies.potential_dirty("source", point(4)));
      PS_CHECK(irrelevant.at("value") == point(4));
      PS_CHECK(demand.replace_bindings(binding(root, 10)).ok());
      auto third = take(demand.request({{"value", point(4)}}, {}, options));
      PS_CHECK(number(third.results.at("value"), 4) == 24 &&
               module.counts()[2] - after[2] == 5 &&
               module.counts()[3] == after[3]);
    }
  } else {
    if (first.ok())
      std::cerr << "unexpected checkpoint success " << mode << ' ' << key
                << '\n';
    PS_CHECK(!first.ok() && first.status().code == ErrorCode::InvalidArgument);
    const auto counts = module.counts();
    PS_CHECK(counts[0] == 1 && counts[1] == 1);
  }
  PS_CHECK(context.cache_statistics().in_flight == 0 &&
           root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
int finite_prefix(double first, double second) {
  Module module;
  PS_CHECK(module.handle);
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->load_plugin(PS_RESULT_CHECKPOINT_FIXTURE).ok() &&
           registry->freeze().ok());
  GraphContext graph(document(0, "fixture.result_checkpoint"));
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 1048576;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  auto demand = take(context.open_demand(plan, binding(root, first, second)));
  auto valid = take(demand.request({{"value", point(0)}}));
  PS_CHECK(number(valid.results.at("value"), 0) == first);
  const auto before = module.counts();
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    auto failed = demand.request({{"value", point(1)}});
    PS_CHECK(!failed.ok() &&
             failed.status().code == ErrorCode::OperationFailed);
    const auto after = module.counts();
    PS_CHECK(after[0] == before[0] + repeat + 1 && after[0] == after[1] &&
             after[2] == before[2] + 2 * (repeat + 1) &&
             after[4] == before[4] + repeat + 1);
  }
  PS_CHECK(context.cache_statistics().in_flight == 0 &&
           root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
int shared_prefix(unsigned mode) {
  Module module;
  PS_CHECK(module.handle);
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->load_plugin(PS_RESULT_CHECKPOINT_FIXTURE).ok() &&
           registry->freeze().ok());
  GraphContext graph(document(mode, "fixture.result_checkpoint"));
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.result_cache_bytes = 1048576;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  auto frozen = take(context.freeze(plan, binding(root)));
  module.gate(1);
  struct Release {
    const Module& module;
    ~Release() { module.gate(2); }
  };
  auto first = std::async(std::launch::async, [&] {
    return context.execute_fragments(frozen, {{"value", point(2)}});
  });
  Release release{module};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!module.gate(0) && std::chrono::steady_clock::now() < deadline &&
         first.wait_for(std::chrono::milliseconds(0)) !=
             std::future_status::ready)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  PS_CHECK(module.gate(0));
  const auto before = module.counts();
  auto second = context.execute_fragments(frozen, {{"value", point(4)}});
  const auto after = module.counts();
  PS_CHECK(before[2] == 3 && before[3] == 0 && after[3] == 1);
  if (mode == 8) {
    PS_CHECK(!second.ok() &&
             second.status().detail.origin == FailureOrigin::Protocol &&
             after[2] == before[2]);
  } else {
    if (!second.ok())
      std::cerr << second.status().message << '\n';
    PS_CHECK(second.ok() &&
             number(second.value().results.at("value"), 4) == 15 &&
             after[2] - before[2] == 2);
    auto dirty =
        take(second.value().dependencies.potential_dirty("source", point(0)));
    PS_CHECK(dirty.at("value") == point(4));
  }
  module.gate(2);
  auto completed = first.get();
  PS_CHECK(
      completed.ok() && number(completed.value().results.at("value"), 2) == 6 &&
      module.counts()[1] == 2 && context.cache_statistics().in_flight == 0 &&
      root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
int cancelled_read() {
  Module module;
  PS_CHECK(module.handle);
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->load_plugin(PS_RESULT_CHECKPOINT_FIXTURE).ok() &&
           registry->freeze().ok());
  GraphContext graph(document(18, "fixture.result_checkpoint"));
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  auto frozen = take(context.freeze(plan, binding(root)));
  CancellationSource cancellation;
  module.gate(1);
  auto pending = std::async(std::launch::async, [&] {
    return context.execute_fragments(frozen, {{"value", point(2)}},
                                     cancellation.token());
  });
  struct Release {
    const Module& module;
    ~Release() { module.gate(2); }
  } release{module};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!module.gate(0) && std::chrono::steady_clock::now() < deadline &&
         pending.wait_for(std::chrono::milliseconds(0)) !=
             std::future_status::ready)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  PS_CHECK(module.gate(0));
  cancellation.cancel();
  module.gate(2);
  auto result = pending.get();
  const auto counts = module.counts();
  PS_CHECK(!result.ok() && result.status().code == ErrorCode::Cancelled &&
           counts[0] == 1 && counts[1] == 1 && counts[2] == 1 &&
           counts[4] == 1 && counts[5] == 0 &&
           context.cache_statistics().in_flight == 0 &&
           root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
}  // namespace
int main() try {
  PS_CHECK(workflow(0) == 0);
  PS_CHECK(workflow(0, false) == 0);
  PS_CHECK(workflow(12) == 0);
  PS_CHECK(shared_prefix(16) == 0);
  PS_CHECK(shared_prefix(8) == 0);
  PS_CHECK(cancelled_read() == 0);
  PS_CHECK(finite_prefix(1, std::numeric_limits<double>::infinity()) == 0);
  PS_CHECK(finite_prefix(1, std::numeric_limits<double>::quiet_NaN()) == 0);
  PS_CHECK(finite_prefix(std::numeric_limits<double>::max(),
                         std::numeric_limits<double>::max()) == 0);
  for (unsigned mode : {1, 2, 3, 4, 5, 6, 7, 9, 11, 13, 14, 15, 17})
    PS_CHECK(workflow(mode) == 0);
  PS_CHECK(workflow(10, true, "fixture.terminal_checkpoint") == 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
