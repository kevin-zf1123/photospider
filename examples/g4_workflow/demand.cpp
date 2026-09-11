#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

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
  document.inputs = {
      {1, "data", data.descriptor(), data.region(), data.layout(), {}},
      {2, "radius", radius.descriptor(), radius.region(), radius.layout(), {}}};
  document.nodes = {{1,
                     "numeric.radius_scatter",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"sum", 1, "value"}};
  auto operations = make_default_operation_registry();
  GraphContext graph(document);
  auto plan = checked(Compiler(operations).compile(graph)).plan;
  ExecutionContext context(operations, {1, false, 8, 4096});
  ExecutionBindings bindings{{{"data", data}, {"radius", radius}}};
  auto demand = checked(context.open_demand(plan, bindings));
  const auto q = checked(
      Footprint::from_regions({5}, {Region({{0, 1}}), Region({{4, 1}})}));
  DemandQuery query{{"sum", q}};
  auto before = checked(demand.request(query));
  auto frozen = checked(demand.freeze());
  bindings.inputs[1].value =
      values<std::int64_t>(ElementType::Int64, {0, 0, 0, 3, 0});
  auto control_edit = checked(demand.replace_bindings(bindings));
  require(control_edit.potential_dirty.at("sum") == q);
  bindings.inputs[0].value =
      values<double>(ElementType::Float64, {1, 2, 3, 9, 5});
  auto data_edit = checked(demand.replace_bindings(bindings));
  require(data_edit.potential_dirty.at("sum") == q);
  auto latest = checked(demand.request(query));
  auto old = checked(context.execute_fragments(frozen, query));
  double current[2]{}, prior[2]{};
  for (unsigned i = 0; i < 2; ++i) {
    const std::vector<std::uint64_t> at{i * 4U};
    require(latest.values.at("sum").read(at, &current[i], 8).ok());
    require(old.values.at("sum").read(at, &prior[i], 8).ok());
  }
  // Direct radius predicate: only source 3 joins source 0/4 at each endpoint.
  require(current[0] == 10 && current[1] == 14 && prior[0] == 1 &&
          prior[1] == 5);
  require(!latest.values.at("sum").read({2}, current, 8).ok());
  require(latest.generation == 3 && old.generation == 0);
  require(demand.release(query).ok());
  require(checked(demand.replace_bindings(bindings)).coverage.empty());
  std::cout << "demand: Q={0,4}, latest=[10,14], frozen=[1,5], "
               "generation=3, accumulated_dirty={0,4}, release=ok\n";
}

void cache_workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto data = values<double>(ElementType::Float64, {1, 2, 3, 0, 5});
  auto radius = values<std::int64_t>(ElementType::Int64, {0, 0, 0, 0, 0});
  WorkflowDocument document;
  document.inputs = {
      {1, "data", data.descriptor(), data.region(), data.layout(), {}},
      {2, "radius", radius.descriptor(), radius.region(), radius.layout(), {}}};
  document.nodes = {{1,
                     "numeric.radius_scatter",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"sum", 1, "value"}};
  auto operations = make_default_operation_registry();
  GraphContext graph(document);
  auto plan = checked(Compiler(operations).compile(graph)).plan;
  ExecutionContext context(operations, {1, false, 8, 4096, 2048});
  ExecutionBindings bindings{{{"data", data}, {"radius", radius}}};
  auto demand = checked(context.open_demand(plan, bindings));
  const auto q = checked(
      Footprint::from_regions({5}, {Region({{0, 1}}), Region({{4, 1}})}));
  const DemandQuery query{{"sum", q}};
  require(checked(demand.request(query)).diagnostics.cache_hits == 0);
  auto warm = checked(demand.request(query));
  require(warm.diagnostics.cache_hits == 2 &&
          warm.diagnostics.operation_timings.empty());
  bindings.inputs[0].value =
      values<double>(ElementType::Float64, {1, 2, 777, 0, 5});
  require(checked(demand.replace_bindings(bindings))
              .potential_dirty.at("sum")
              .empty());
  require(checked(demand.request(query)).diagnostics.cache_hits == 2);
  bindings.inputs[1].value =
      values<std::int64_t>(ElementType::Int64, {0, 0, 0, 3, 0});
  require(
      checked(demand.replace_bindings(bindings)).potential_dirty.at("sum") ==
      q);
  auto changed = checked(demand.request(query));
  require(changed.diagnostics.cache_hits == 0);
  // Direct radius predicate: source 3 now contributes zero to both endpoints.
  double first = 0, last = 0;
  require(changed.values.at("sum").read({0}, &first, 8).ok());
  require(changed.values.at("sum").read({4}, &last, 8).ok());
  require(first == 1 && last == 5);
  context.clear_result_cache();
  const auto data3 = checked(Footprint::from_regions({5}, {Region({{3, 1}})}));
  require(context.cache_statistics().retained_bytes == 0);
  require(
      checked(changed.dependencies.potential_dirty("data", data3)).at("sum") ==
      q);
  std::cout
      << "cache: warm_hits=2, unrelated_edit_hits=2, control_edit_hits=0, "
         "values=[1,5], cleared_pixels=0, data3_dirty={0,4}\n";
}
