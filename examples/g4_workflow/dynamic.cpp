#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T checked(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
template <class T>
Value data(ElementType type, std::vector<std::uint64_t> shape,
           const std::vector<T>& samples) {
  auto output = checked(MutableValue::allocate(
      {type, shape}, Region::whole(shape), BufferAllocator{}));
  if (output.size() != samples.size() * sizeof(T))
    throw std::runtime_error("fixture size mismatch");
  std::memcpy(output.data(), samples.data(), output.size());
  return checked(std::move(output).publish());
}
double first(const ExecutionResult& result, const std::string& name) {
  double value = 0;
  std::memcpy(&value, result.values.at(name).bytes().data(), sizeof(value));
  return value;
}
void radius_workflow(const std::shared_ptr<OperationRegistry>& registry) {
  const auto source =
      data(ElementType::Float64, {4}, std::vector<double>{1, 2, 3, 4});
  const auto radius =
      data(ElementType::Int64, {4}, std::vector<std::int64_t>{0, 0, 0, 0});
  WorkflowDocument document;
  document.inputs = {
      {1, "source", source.descriptor(), source.region(), source.layout(), {}},
      {2, "radius", radius.descriptor(), radius.region(), radius.layout(), {}}};
  document.nodes = {{1,
                     "numeric.radius_scatter",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}},
                    {2,
                     "numeric.radius_gather",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"scatter", 1, "value"}, {"gather", 2, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  PlanningOptions options;
  options.output_regions = {{"scatter", Region({{0, 1}})},
                            {"gather", Region({{0, 1}})}};
  auto plan = checked(compiler.compile(graph, options)).plan;
  InputSnapshotStore store({128, 1});
  auto old = checked(store.import_value(radius));
  auto writer = checked(MutableValue::allocate(
      radius.descriptor(), Region({{3, 1}}), BufferAllocator{}));
  const std::int64_t three = 3;
  std::memcpy(writer.data(), &three, 8);
  auto changed =
      checked(store.patch(old, checked(std::move(writer).publish())));
  ExecutionBindings old_bindings{
      {{"source", source},
       {"radius", {}, {}, std::make_shared<const InputSnapshot>(old)}}};
  ExecutionBindings new_bindings{
      {{"source", source},
       {"radius", {}, {}, std::make_shared<const InputSnapshot>(changed)}}};
  ExecutionContext execution(registry, {1, false, 8, 2048});
  auto frozen = checked(execution.freeze(plan, old_bindings));
  const auto before = checked(execution.execute(plan, old_bindings));
  const auto after = checked(execution.execute(plan, new_bindings));
  const auto pinned = checked(execution.execute(frozen));
  if (first(before, "scatter") != 1 || first(after, "scatter") != 5 ||
      first(after, "gather") != 1 || first(pinned, "scatter") != 1)
    throw std::runtime_error("radius independent oracle failed");
  std::cout
      << "radius: scatter_before=1, scatter_after=5, gather=1, frozen=1\n";
  const auto edited = checked(Footprint::from_regions({4}, {Region({{3, 1}})}));
  const auto output = checked(Footprint::from_regions({4}, {Region({{0, 1}})}));
  const auto dirty =
      checked(before.dependencies.potential_dirty("radius", edited));
  const auto new_data =
      checked(after.dependencies.potential_dirty("source", edited));
  const auto frozen_data =
      checked(pinned.dependencies.potential_dirty("source", edited));
  const auto restricted =
      checked(before.dependencies.restrict({{"scatter", output}}));
  if (dirty.at("scatter") != output || !dirty.at("gather").empty() ||
      new_data.at("scatter") != output || !frozen_data.at("scatter").empty() ||
      restricted.coverage().size() != 1)
    throw std::runtime_error("runtime dependency evidence oracle failed");
  std::cout << "dependencies: radius[3] -> scatter{0}, gather{}, "
               "new_data_edge=present, frozen_data_edge=absent\n";
}
}  // namespace
void dynamic_workflow() {
  auto registry = make_default_operation_registry();
  radius_workflow(registry);
}
