#include <dlfcn.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/result_input_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using result_input_fixture::take;
struct Module {
  void* handle = dlopen(PS_RESULT_BLOCK_FIXTURE, RTLD_NOW | RTLD_LOCAL);
  ~Module() {
    if (handle)
      dlclose(handle);
  }
  std::array<std::uint64_t, 3> counts() const {
    auto function = reinterpret_cast<void (*)(std::uint64_t*)>(
        dlsym(handle, "ps_result_block_counts"));
    if (!function)
      throw std::runtime_error("missing C block counts");
    std::array<std::uint64_t, 3> result{};
    function(result.data());
    return result;
  }
};
SchemaTemplate schema() {
  SchemaTemplate result;
  result.id = "fixture.block.input";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ElementType::Float64, {6}};
  result.tensors.push_back(std::move(tensor));
  return result;
}
ExecutionBindings binding(const ResourceBudget& root,
                          const std::array<double, 6>& numbers) {
  std::vector<std::uint8_t> bytes(sizeof(numbers));
  std::memcpy(bytes.data(), numbers.data(), bytes.size());
  auto value = take(Value::create({ElementType::Float64, {6}},
                                  Region::whole({6}), {0, {8}}, bytes));
  auto metadata = schema();
  return {{{"source", result_input_fixture::source(root, value, &metadata)}}};
}
int workflow(unsigned mode, bool cache, bool exhaust_cache_work = false) {
  Module module;
  PS_CHECK(module.handle);
  ResourceBudget root;
  {
    auto registry = std::make_shared<OperationRegistry>();
    auto loaded = registry->load_plugin(PS_RESULT_BLOCK_FIXTURE);
    if (!loaded.ok())
      std::cerr << loaded.message << '\n';
    PS_CHECK(loaded.ok() && registry->freeze().ok());
    PS_CHECK(take(registry->find_traits("fixture.result_block"))
                 .share_blocks_across_outputs);
    WorkflowDocument doc;
    WorkflowInputDeclaration input;
    input.id = 1;
    input.name = "source";
    input.result_schema = std::make_shared<const SchemaTemplate>(schema());
    doc.inputs = {input};
    doc.nodes = {{10,
                  "fixture.result_block",
                  {WorkflowInputReference{1}},
                  {{"mode", static_cast<std::int64_t>(mode)}}}};
    doc.outputs = {{"value", 10, "value"}, {"control", 10, "control"}};
    GraphContext graph(doc);
    PlanningOptions planning;
    planning.tile_extent = 1;
    auto plan = take(Compiler(registry).compile(graph, planning)).plan;
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = cache ? 1048576 : 0;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    std::array<double, 6> numbers{0, 1, 0x1p54, 4, 5, 6};
    auto demand = take(context.open_demand(plan, binding(root, numbers)));
    TileRequest point{false, take(TileSet::from_indices(root, {5}))};
    DemandQuery query{{"value", point}};
    const auto before = module.counts();
    ExecutionOptions options;
    if (exhaust_cache_work)
      options.maximum_dependency_cache_work = 1;
    const bool reuse = cache && !exhaust_cache_work;
    if (mode == 0) {
      auto initial = demand.request(query, {}, options);
      if (!initial.ok())
        std::cerr << "C block: " << initial.status().message << '\n';
      PS_CHECK(initial.ok());
      auto other = take(demand.request({{"control", point}}, {}, options));
      double control_value = 0;
      PS_CHECK(result_input_fixture::read(other.results.at("control"), {0, 5},
                                          &control_value, 8)
                   .ok() &&
               control_value == 0x1p54 + 16);
      const auto control_step = plan.outputs().at("control");
      auto control_record = take(other.dependencies.tile_record(
          {control_step, plan.steps()[control_step].output_index, 5}));
      PS_CHECK(control_record && control_record->inputs.size() == 1);
      PS_CHECK(control_record->inputs[0].whole &&
               control_record->inputs[0].tiles.empty());
      PS_CHECK(std::holds_alternative<PlanWorkflowInput>(
          control_record->inputs[0].producer));
      PS_CHECK(std::get<PlanWorkflowInput>(control_record->inputs[0].producer)
                   .declaration_index == 0);
      PS_CHECK(control_record->inputs[0].producer_output == 0);
      PS_CHECK(module.counts()[2] - before[2] == (reuse ? 6 : 12));
      numbers[0] = 1;
      auto replacement = take(demand.replace_bindings(binding(root, numbers)));
      PS_CHECK(replacement.potential_dirty.at("value") == point.tiles);
      PS_CHECK(replacement.potential_dirty.at("control") == point.tiles);
      auto changed = take(demand.request(query, {}, options));
      volatile double expected = 0;
      for (const auto number : numbers)
        expected = expected + number;
      double actual = 0;
      PS_CHECK(result_input_fixture::read(changed.results.at("value"), {0, 5},
                                          &actual, 8)
                   .ok() &&
               actual == expected);
      const auto value_step = plan.outputs().at("value");
      auto value_record = take(changed.dependencies.tile_record(
          {value_step, plan.steps()[value_step].output_index, 5}));
      PS_CHECK(value_record && value_record->inputs.size() == 1);
      PS_CHECK(value_record->inputs[0].whole &&
               value_record->inputs[0].tiles.empty());
      PS_CHECK(std::holds_alternative<PlanWorkflowInput>(
          value_record->inputs[0].producer));
      PS_CHECK(std::get<PlanWorkflowInput>(value_record->inputs[0].producer)
                   .declaration_index == 0);
      PS_CHECK(value_record->inputs[0].producer_output == 0);
      PS_CHECK(module.counts()[2] - before[2] == (reuse ? 12 : 18));
    } else {
      for (unsigned repeat = 0; repeat < 2; ++repeat) {
        auto failed = demand.request(query, {}, options);
        auto expected = mode <= 8    ? ErrorCode::InvalidArgument
                        : mode == 10 ? ErrorCode::ResourceExhausted
                                     : ErrorCode::OperationFailed;
        if (failed.ok() || failed.status().code != expected)
          std::cerr << "C block mode " << mode << ": "
                    << failed.status().message << '\n';
        PS_CHECK(!failed.ok() && failed.status().code == expected &&
                 module.counts()[2] ==
                     before[2] + (mode == 7 ? 0 : repeat + 1));
        PS_CHECK(module.counts()[0] == module.counts()[1]);
      }
    }
    PS_CHECK(module.counts()[0] == module.counts()[1] &&

             root.statistics().live[ResourceKind::Queue] == 0);
  }
  for (auto bytes : root.statistics().live.values)
    PS_CHECK(bytes == 0);
  return 0;
}
}  // namespace
int main() try {
  PS_CHECK(workflow(0, true) == 0);
  PS_CHECK(workflow(0, false) == 0);
  PS_CHECK(workflow(0, true, true) == 0);
  for (unsigned mode = 7; mode <= 11; ++mode)
    PS_CHECK(workflow(mode, true) == 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
