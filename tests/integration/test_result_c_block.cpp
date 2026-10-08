#include <dlfcn.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using numeric_result_fixture::take;
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
  return {{{"source", numeric_result_fixture::source(root, value, &metadata)}}};
}
int workflow(unsigned mode, bool cache) {
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
    auto plan = take(Compiler(registry).compile(graph)).plan;
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = cache ? 1048576 : 0;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    std::array<double, 6> numbers{0, 1, 0x1p54, 4, 5, 6};
    auto demand = take(context.open_demand(plan, binding(root, numbers)));
    auto point = take(Footprint::from_regions({6}, {Region({{5, 1}})}));
    DemandQuery query{{"value", point}};
    const auto before = module.counts();
    if (mode == 0) {
      auto initial = demand.request(query);
      if (!initial.ok())
        std::cerr << "C block: " << initial.status().message << '\n';
      PS_CHECK(initial.ok());
      auto other = take(demand.request({{"control", point}}));
      double control_value = 0;
      PS_CHECK(numeric_result_fixture::read(other.results.at("control"), {5},
                                            &control_value, 8)
                   .ok() &&
               control_value == 0x1p54 + 16);
      auto control_dirty = take(other.dependencies.potential_dirty(
          "source", take(Footprint::all({6})), 2));
      auto data_dirty = take(other.dependencies.potential_dirty(
          "source", take(Footprint::all({6})), 1));
      PS_CHECK(control_dirty.at("control") == point &&
               data_dirty.at("control").empty());
      numbers[0] = 1;
      PS_CHECK(demand.replace_bindings(binding(root, numbers)).ok());
      auto changed = take(demand.request(query));
      volatile double expected = 0;
      for (const auto number : numbers)
        expected = expected + number;
      double actual = 0;
      PS_CHECK(numeric_result_fixture::read(changed.results.at("value"), {5},
                                            &actual, 8)
                   .ok() &&
               actual == expected);
      PS_CHECK(take(changed.dependencies.source_support()).at("source") ==
               take(Footprint::all({6})));
      auto dirty = take(changed.dependencies.potential_dirty(
          "source", take(Footprint::from_regions({6}, {Region({{0, 1}})}))));
      PS_CHECK(dirty.at("value") == point);
    } else {
      for (unsigned repeat = 0; repeat < 2; ++repeat) {
        auto failed = demand.request(query);
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
             context.cache_statistics().in_flight == 0 &&
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
  for (unsigned mode = 7; mode <= 11; ++mode)
    PS_CHECK(workflow(mode, true) == 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
