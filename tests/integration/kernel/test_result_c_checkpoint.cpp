#include <dlfcn.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/result_input_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using result_input_fixture::take;
struct Module {
  void* handle = dlopen(PS_RESULT_CHECKPOINT_FIXTURE, RTLD_NOW | RTLD_LOCAL);
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  mutable bool entered = false, released = true;
  Module() {
    if (!handle)
      return;
    auto set_gate = reinterpret_cast<void (*)(void (*)(void*), void*)>(
        dlsym(handle, "ps_result_checkpoint_set_gate"));
    if (!set_gate)
      throw std::runtime_error("missing C checkpoint gate");
    set_gate(
        [](void* raw) {
          auto& self = *static_cast<Module*>(raw);
          std::unique_lock<std::mutex> lock(self.mutex);
          if (self.entered)
            return;
          self.entered = true;
          self.condition.notify_all();
          self.condition.wait(lock, [&] { return self.released; });
        },
        this);
  }
  ~Module() {
    if (handle) {
      auto set_gate = reinterpret_cast<void (*)(void (*)(void*), void*)>(
          dlsym(handle, "ps_result_checkpoint_set_gate"));
      if (set_gate)
        set_gate(nullptr, nullptr);
      dlclose(handle);
    }
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
    std::lock_guard<std::mutex> lock(mutex);
    if (action == 1) {
      entered = false;
      released = false;
    } else if (action == 2) {
      released = true;
      condition.notify_all();
    }
    return entered;
  }
  bool wait() const {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(5),
                              [&] { return entered; });
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
  auto value = take(TensorSpan::create({ElementType::Float64, {5}},
                                       Region::whole({5}), {0, {8}}, bytes));
  auto metadata = schema();
  return {{{"source", result_input_fixture::source(root, value, &metadata)}}};
}
TileRequest point(const ResourceBudget& root, std::uint64_t index) {
  return {{{0, take(TileSet::from_indices(root, {index}))}}};
}
PlanningOptions planning() {
  PlanningOptions options;
  options.tile_extent = 1;
  return options;
}
double number(const ResultRef& result, std::uint64_t at) {
  double value = 0;
  auto status = result_input_fixture::read(result, {0, at}, &value, 8);
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
  auto plan = take(Compiler(registry).compile(graph, planning())).plan.value();
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
  auto first = demand.request({{"value", point(root, 2)}}, {}, options);
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
    auto second = demand.request({{"value", point(root, 4)}}, {}, options);
    auto after = module.counts();
    {
      if (!second.ok())
        std::cerr << second.status().message << '\n';
      PS_CHECK(second.ok() &&
               number(second.value().results.at("value"), 4) == 15 &&
               after[0] == 2 && after[1] == 2 && after[2] - before[2] == 5 &&
               after[3] == 0);
      auto record = take(second.value().dependencies.tile_record(
          {plan.output("value").record_index, 0, 0, 4}));
      PS_CHECK(record && record->inputs.size() == 1 &&
               record->inputs[0].tensor_slot == 0 &&
               record->inputs[0].tiles == take(TileSet::range(root, 0, 5)));
      PS_CHECK(std::holds_alternative<PlanWorkflowInput>(
          record->inputs[0].producer));
      PS_CHECK(std::get<PlanWorkflowInput>(record->inputs[0].producer)
                   .declaration_index == 0);
      PS_CHECK(record->inputs[0].producer_output == 0);
      auto changed = take(demand.replace_bindings(binding(root, 10)));
      PS_CHECK(changed.potential_dirty.at("value").members.at(0) ==
               take(TileSet::from_indices(root, {2, 4})));
      auto third =
          take(demand.request({{"value", point(root, 4)}}, {}, options));
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
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
int finite_prefix(double first, double second) {
  Module module;
  PS_CHECK(module.handle);
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->load_plugin(PS_RESULT_CHECKPOINT_FIXTURE).ok() &&
           registry->freeze().ok());
  GraphContext graph(document(0, "fixture.result_checkpoint"));
  auto plan = take(Compiler(registry).compile(graph, planning())).plan.value();
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 1048576;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  auto demand = take(context.open_demand(plan, binding(root, first, second)));
  auto valid = take(demand.request({{"value", point(root, 0)}}));
  PS_CHECK(number(valid.results.at("value"), 0) == first);
  const auto before = module.counts();
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    auto failed = demand.request({{"value", point(root, 1)}});
    PS_CHECK(!failed.ok() &&
             failed.status().code == ErrorCode::OperationFailed);
    const auto after = module.counts();
    PS_CHECK(after[0] == before[0] + repeat + 1 && after[0] == after[1] &&
             after[2] == before[2] + 2 * (repeat + 1) &&
             after[4] == before[4] + repeat + 1);
  }
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
int shared_prefix(unsigned mode, bool restore_prefix = true) {
  Module module;
  PS_CHECK(module.handle);
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->load_plugin(PS_RESULT_CHECKPOINT_FIXTURE).ok() &&
           registry->freeze().ok());
  GraphContext graph(document(mode, "fixture.result_checkpoint"));
  auto plan = take(Compiler(registry).compile(graph, planning())).plan.value();
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.result_cache_bytes = 1048576;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  auto run_plan = plan;
  ExecutionBindings run_bindings = binding(root);
  module.gate(1);
  struct Release {
    const Module& module;
    ~Release() { module.gate(2); }
  };
  auto first = std::async(std::launch::async, [&] {
    return context.execute_fragments(run_plan, run_bindings,
                                     {{"value", point(root, 2)}});
  });
  Release release{module};
  PS_CHECK(module.wait());
  const auto before = module.counts();
  ExecutionOptions options;
  if (!restore_prefix)
    options.maximum_dependency_cache_work = 0;
  auto second = context.execute_fragments(
      run_plan, run_bindings, {{"value", point(root, 2)}}, {}, options);
  const auto after = module.counts();
  PS_CHECK(before[2] == 3 && before[3] == 0 &&
           after[3] == (restore_prefix ? 1 : 0));
  if (mode == 8 && restore_prefix) {
    PS_CHECK(!second.ok() &&
             second.status().detail.origin == FailureOrigin::Protocol &&
             second.status().reason == FailureReason::UnauthorizedRead &&
             after[2] == before[2]);
  } else {
    if (!second.ok())
      std::cerr << second.status().message << '\n';
    PS_CHECK(second.ok() &&
             number(second.value().results.at("value"), 2) == 6 &&
             after[2] - before[2] == (restore_prefix ? 0 : 3));
    auto record = take(second.value().dependencies.tile_record(
        {plan.output("value").record_index, 0, 0, 2}));
    PS_CHECK(record && record->inputs.size() == 1 &&
             record->inputs[0].tensor_slot == 0 &&
             record->inputs[0].tiles == take(TileSet::range(root, 0, 5)));
    PS_CHECK(
        std::holds_alternative<PlanWorkflowInput>(record->inputs[0].producer));
    PS_CHECK(std::get<PlanWorkflowInput>(record->inputs[0].producer)
                 .declaration_index == 0);
    PS_CHECK(record->inputs[0].producer_output == 0);
  }
  module.gate(2);
  auto completed = first.get();
  PS_CHECK(completed.ok() &&
           number(completed.value().results.at("value"), 2) == 6 &&
           module.counts()[1] == 2 &&
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
  auto plan = take(Compiler(registry).compile(graph, planning())).plan.value();
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  auto run_plan = plan;
  ExecutionBindings run_bindings = binding(root);
  CancellationSource cancellation;
  module.gate(1);
  auto pending = std::async(std::launch::async, [&] {
    return context.execute_fragments(run_plan, run_bindings,
                                     {{"value", point(root, 2)}},
                                     cancellation.token());
  });
  struct Release {
    const Module& module;
    ~Release() { module.gate(2); }
  } release{module};
  PS_CHECK(module.wait());
  cancellation.cancel();
  module.gate(2);
  auto result = pending.get();
  const auto counts = module.counts();
  PS_CHECK(!result.ok() && result.status().code == ErrorCode::Cancelled &&
           counts[0] == 1 && counts[1] == 1 && counts[2] == 1 &&
           counts[4] == 1 && counts[5] == 0 &&

           root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
}  // namespace
int main() try {
  PS_CHECK(workflow(0) == 0);
  PS_CHECK(workflow(0, false) == 0);
  PS_CHECK(workflow(12) == 0);
  PS_CHECK(shared_prefix(16) == 0);
  PS_CHECK(shared_prefix(16, false) == 0);
  PS_CHECK(shared_prefix(8) == 0);
  PS_CHECK(cancelled_read() == 0);
  PS_CHECK(finite_prefix(1, std::numeric_limits<double>::infinity()) == 0);
  PS_CHECK(finite_prefix(1, std::numeric_limits<double>::quiet_NaN()) == 0);
  PS_CHECK(finite_prefix(std::numeric_limits<double>::max(),
                         std::numeric_limits<double>::max()) == 0);
  for (unsigned mode : {1, 2, 3, 4, 5, 6, 7, 9, 11, 13, 14, 15, 17})
    PS_CHECK(workflow(mode) == 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
