#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../../../examples/numeric_workflow/result_fixture.hpp"
#include "photospider/photospider.hpp"

namespace {
template <class T>
T checked(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
void require(bool condition) {
  if (!condition)
    throw std::runtime_error("demand independent oracle failed");
}
template <class T>
ps::Value values(ps::ElementType type, const std::vector<T>& samples) {
  auto writer = checked(ps::MutableValue::allocate(
      {type, {samples.size()}}, ps::Region::whole({samples.size()}),
      ps::BufferAllocator{}));
  std::memcpy(writer.data(), samples.data(), writer.size());
  return checked(std::move(writer).publish());
}
}  // namespace
void demand_workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto data = values<double>(ElementType::Float64, {1, 2, 3, 0, 5});
  auto radius = values<std::int64_t>(ElementType::Int64, {0, 0, 0, 0, 0});
  WorkflowDocument document;
  numeric_result_fixture::declare_sources(&document, {data, radius});
  document.inputs[0].name = "data";
  document.inputs[1].name = "radius";
  document.nodes = {{1,
                     "numeric.radius_scatter",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"sum", 1, "value"}};
  auto operations = make_default_operation_registry();
  GraphContext graph(document);
  auto plan = checked(Compiler(operations).compile(graph)).plan;
  ExecutionContext context(operations, {1, false, 8, 4096});
  const auto root = checked(context.resource_budget());
  ExecutionBindings bindings{
      {{"data", numeric_result_fixture::source(root, data)},
       {"radius", numeric_result_fixture::source(root, radius)}}};
  auto demand = checked(context.open_demand(plan, bindings));
  const auto q = checked(
      Footprint::from_regions({5}, {Region({{0, 1}}), Region({{4, 1}})}));
  DemandQuery query{{"sum", q}};
  auto before = checked(demand.request(query));
  auto frozen = checked(demand.freeze());
  bindings.inputs[1].result = numeric_result_fixture::source(
      root, values<std::int64_t>(ElementType::Int64, {0, 0, 0, 3, 0}));
  auto control_edit = checked(demand.replace_bindings(bindings));
  require(control_edit.potential_dirty.at("sum") == q);
  bindings.inputs[0].result = numeric_result_fixture::source(
      root, values<double>(ElementType::Float64, {1, 2, 3, 9, 5}));
  auto data_edit = checked(demand.replace_bindings(bindings));
  require(data_edit.potential_dirty.at("sum") == q);
  auto latest = checked(demand.request(query));
  auto old = checked(context.execute_fragments(frozen, query));
  double current[2]{}, prior[2]{};
  for (unsigned i = 0; i < 2; ++i) {
    const std::vector<std::uint64_t> at{i * 4U};
    require(numeric_result_fixture::read(latest.results.at("sum"), at,
                                         &current[i], 8)
                .ok());
    require(
        numeric_result_fixture::read(old.results.at("sum"), at, &prior[i], 8)
            .ok());
  }
  // Direct radius predicate: only source 3 joins source 0/4 at each endpoint.
  require(current[0] == 10 && current[1] == 14 && prior[0] == 1 &&
          prior[1] == 5);
  require(
      !numeric_result_fixture::read(latest.results.at("sum"), {2}, current, 8)
           .ok());
  require(latest.generation == 3 && old.generation == 0);
  require(demand.release(query).ok());
  require(checked(demand.replace_bindings(bindings)).coverage.empty());
  std::cout << "demand: Q={0,4}, latest=[10,14], frozen=[1,5], "
               "generation=3, accumulated_dirty={0,4}, release=ok\n";
}

void radius_retention_workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto data = values<double>(ElementType::Float64, {1, 2, 3, 0, 5});
  auto radius = values<std::int64_t>(ElementType::Int64, {0, 0, 0, 0, 0});
  WorkflowDocument document;
  numeric_result_fixture::declare_sources(&document, {data, radius});
  document.inputs[0].name = "data";
  document.inputs[1].name = "radius";
  document.nodes = {{1,
                     "numeric.radius_scatter",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"sum", 1, "value"}};
  auto operations = make_default_operation_registry();
  GraphContext graph(document);
  auto plan = checked(Compiler(operations).compile(graph)).plan;
  ExecutionContext context(operations, {1, false, 8, 4096, 2048});
  const auto root = checked(context.resource_budget());
  ExecutionBindings bindings{
      {{"data", numeric_result_fixture::source(root, data)},
       {"radius", numeric_result_fixture::source(root, radius)}}};
  auto demand = checked(context.open_demand(plan, bindings));
  const auto q = checked(
      Footprint::from_regions({5}, {Region({{0, 1}}), Region({{4, 1}})}));
  const DemandQuery query{{"sum", q}};
  auto first = checked(demand.request(query));
  auto warm = checked(demand.request(query));
  require(warm.results.at("sum").object_id() ==
              first.results.at("sum").object_id() &&
          warm.diagnostics.operation_timings.empty());
  bindings.inputs[0].result = numeric_result_fixture::source(
      root, values<double>(ElementType::Float64, {1, 2, 777, 0, 5}));
  require(checked(demand.replace_bindings(bindings))
              .potential_dirty.at("sum")
              .empty());
  auto unchanged = checked(demand.request(query));
  require(unchanged.results.at("sum").object_id() !=
          warm.results.at("sum").object_id());
  bindings.inputs[1].result = numeric_result_fixture::source(
      root, values<std::int64_t>(ElementType::Int64, {0, 0, 0, 3, 0}));
  require(
      checked(demand.replace_bindings(bindings)).potential_dirty.at("sum") ==
      q);
  auto changed = checked(demand.request(query));
  require(changed.results.at("sum").object_id() !=
          unchanged.results.at("sum").object_id());
  // Direct radius predicate: source 3 now contributes zero to both endpoints.
  double first_value = 0, last = 0;
  require(numeric_result_fixture::read(changed.results.at("sum"), {0},
                                       &first_value, 8)
              .ok());
  require(numeric_result_fixture::read(changed.results.at("sum"), {4}, &last, 8)
              .ok());
  require(first_value == 1 && last == 5);
  context.clear_result_cache();
  const auto data3 = checked(Footprint::from_regions({5}, {Region({{3, 1}})}));
  require(context.cache_statistics().retained_bytes == 0);
  require(
      checked(changed.dependencies.potential_dirty("data", data3)).at("sum") ==
      q);
  std::cout
      << "radius retention: same_frozen_object=yes, warm_operations=0, "
         "new_generation_recomputed=yes, values=[1,5], data3_dirty={0,4}\n";
}
