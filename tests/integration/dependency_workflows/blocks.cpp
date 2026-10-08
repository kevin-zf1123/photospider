#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "support/dependency_workflow_fixture.hpp"

/** @brief Checks incoming-state keys and valid reuse after reconvergence. */
void block_cache_workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  WorkflowDocument doc;
  doc.inputs = {dependency_fixture::declaration(
      1, "x", dependency_fixture::schema(ElementType::Float64, 6))};
  doc.nodes = {{1,
                "numeric.ordered_scan",
                {WorkflowInputReference{1}},
                {{"block_size", INT64_C(1)}}}};
  doc.outputs = {{"y", 1, "value"}};
  auto registry = make_default_operation_registry();
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {1, false, 8, 4096, 512});
  const auto root = dependency_fixture::take(context.resource_budget());
  const auto input = [&](const std::vector<double>& values) {
    ExecutionBinding binding;
    binding.name = "x";
    binding.result = dependency_fixture::numbers(root, values);
    return ExecutionBindings{{binding}};
  };
  std::vector<double> numbers{0, 1, 0x1p54, 4, 5, 6};
  auto demand = context.open_demand(plan, input(numbers)).take_value();
  auto point = [](std::uint64_t at) {
    return Footprint::from_regions({6}, {Region({{at, 1}})}).take_value();
  };
  auto first = demand.request({{"y", point(5)}});
  if (!first.ok())
    throw std::runtime_error(first.status().message);
  numbers[0] = 1;
  auto edit = demand.replace_bindings(input(numbers));
  if (!edit.ok())
    throw std::runtime_error(edit.status().message);
  auto changed = demand.request({{"y", point(5)}});
  if (!changed.ok())
    throw std::runtime_error(changed.status().message);
  volatile double expected = 0;
  for (const auto number : numbers)
    expected = expected + number;
  const auto actual =
      dependency_fixture::number(changed.value().results.at("y"), 5);
  auto prefix = dependency_fixture::take(demand.request({{"y", point(1)}}));
  const auto short_value =
      dependency_fixture::number(prefix.results.at("y"), 1);
  if (actual != expected || short_value != 2 ||
      changed.value().dependencies.source_support().value().at("x") !=
          Footprint::all({6}).value())
    throw std::runtime_error("block reconvergence oracle failed");
  std::cout << "scan edit reconvergence and prefix oracle passed\n";
}
