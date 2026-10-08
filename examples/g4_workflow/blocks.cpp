#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "result_support.hpp"  // NOLINT(build/include_subdir)

/** @brief Checks incoming-state keys and valid reuse after reconvergence. */
void block_cache_workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  WorkflowDocument doc;
  doc.inputs = {g4_result::declaration(
      1, "x", g4_result::schema(ElementType::Float64, 6))};
  doc.nodes = {{1,
                "numeric.ordered_scan",
                {WorkflowInputReference{1}},
                {{"block_size", INT64_C(1)}}}};
  doc.outputs = {{"y", 1, "value"}};
  auto registry = make_default_operation_registry();
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, {1, false, 8, 4096, 512});
  const auto root = g4_result::take(context.resource_budget());
  const auto input = [&](const std::vector<double>& values) {
    ExecutionBinding binding;
    binding.name = "x";
    binding.result = g4_result::numbers(root, values);
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
  const auto actual = g4_result::number(changed.value().results.at("y"), 5);
  auto prefix = g4_result::take(demand.request({{"y", point(1)}}));
  const auto short_value = g4_result::number(prefix.results.at("y"), 1);
  if (first.value().diagnostics.block_cache_misses != 6 ||
      changed.value().diagnostics.block_cache_hits != 3 ||
      changed.value().diagnostics.block_cache_misses != 3 ||
      actual != expected || short_value != 2 ||
      changed.value().dependencies.source_support().value().at("x") !=
          Footprint::all({6}).value())
    throw std::runtime_error("block reconvergence oracle failed");
  std::cout << "blocks: first_misses=6, edit_misses=3, edit_hits=3, "
               "prefix1=2, source_support=all\n";
}
