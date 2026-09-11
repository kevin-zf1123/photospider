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
void stmap_workflow(const std::shared_ptr<OperationRegistry>& registry) {
  const auto facets =
      std::vector<ValueFacet>{checked(encode_semantic(rgba_semantics()))};
  const auto map =
      data(ElementType::Float64, {1, 1, 2}, std::vector<double>{0, .5});
  WorkflowDocument document;
  document.inputs = {
      {1,
       "source",
       {ElementType::Float32, {1, 1024, 4}},
       Region::whole({1, 1024, 4}),
       {0, {16384, 16, 4}},
       facets},
      {2, "map", map.descriptor(), map.region(), map.layout(), {}}};
  document.nodes = {
      {1, "core.identity", {WorkflowInputReference{2}}, {}},
      {2,
       "image.stmap",
       {WorkflowInputReference{1}, WorkflowNodeOutput{1, "value"}},
       {{"boundary", std::string("wrap")}}}};
  document.outputs = {{"sample", 2, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto plan = checked(compiler.compile(graph)).plan;
  std::vector<std::uint64_t> addresses;
  auto source = std::make_shared<RegionalSource>();
  source->descriptor = document.inputs[0].descriptor;
  source->facets = facets;
  source->read = [&](const Region& region, std::uint8_t* bytes,
                     std::uint64_t size, const BufferAllocator&,
                     const CancellationToken&) {
    const auto x = region.dimensions()[1].offset;
    if (size != 16 || region.dimensions()[1].extent != 1 ||
        (x != 0 && x != 1023))
      return Result<Region>(Status::failure(ErrorCode::OperationFailed,
                                            "unexpected source read"));
    addresses.push_back(x);
    const float pixel[] = {x == 0 ? .25F : .75F, 0, 0, 1};
    std::memcpy(bytes, pixel, sizeof(pixel));
    return Result<Region>(region);
  };
  ExecutionContext execution(registry, {1, false, 8, 512});
  auto result = checked(
      execution.execute(plan, {{{"source", {}, source, {}}, {"map", map}}}));
  float red = 0;
  std::memcpy(&red, result.values.at("sample").bytes().data(), 4);
  if (red != .5F || addresses != std::vector<std::uint64_t>({0, 1023}) ||
      result.diagnostics.source_read_bytes != 32)
    throw std::runtime_error("STMap independent oracle failed");
  std::cout << "stmap: red=0.5, source_pixels=[0,1023], source_bytes=32\n";
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
}
}  // namespace
void dynamic_workflow() {
  auto registry = make_default_operation_registry();
  stmap_workflow(registry);
  radius_workflow(registry);
}
