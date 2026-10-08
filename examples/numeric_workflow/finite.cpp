#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
void require(ps::Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
ps::ResultRef source(const ps::ResourceBudget& root,
                     const ps::SchemaTemplate& schema,
                     const std::vector<double>& data) {
  auto builder = take(ps::ResultBuilder::start(root, schema, "finite.input"));
  require(builder.bind_descriptor_relation(
      take(ps::ResultRelation::cartesian(root, 1, {}))));
  require(builder.publish_tensor(
      0, ps::Region::whole({2, 3}),
      ps::ByteView(reinterpret_cast<const std::uint8_t*>(data.data()),
                   data.size() * 8),
      take(ps::ResultRelation::cartesian(root, data.size(), {})),
      {true, true, true, true}));
  return take(builder.seal());
}
double sample(const ps::ResultRef& result,
              const std::vector<std::uint64_t>& at) {
  double number;
  require(result.read_tensor(take(result.descriptor()), 0, at, &number, 8));
  return number;
}
}  // namespace
int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  try {
    auto registry = make_default_operation_registry();
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    auto root = take(context.resource_budget());
    SchemaTemplate schema;
    schema.id = "example.finite.input";
    ResultTensorSpec tensor;
    tensor.key = "data";
    tensor.descriptor = {ElementType::Float64, {2, 3}};
    schema.tensors.push_back(std::move(tensor));
    auto a = source(root, schema, {-0., -3, 4, 5, -2, 6});
    auto b = source(root, schema, {0., 2, -4, 3, -7, 9});
    WorkflowDocument document;
    for (std::uint64_t id = 1; id <= 2; ++id) {
      WorkflowInputDeclaration input;
      input.id = id;
      input.name = id == 1 ? "a" : "b";
      input.result_schema = std::make_shared<SchemaTemplate>(schema);
      document.inputs.push_back(std::move(input));
    }
    document.nodes = {
        {3, "numeric.abs", {WorkflowInputReference{1}}, {}},
        {4,
         "numeric.minimum",
         {WorkflowNodeOutput{3, "value"}, WorkflowInputReference{2}},
         {}},
        {5,
         "numeric.maximum",
         {WorkflowNodeOutput{4, "value"}, WorkflowNodeOutput{3, "value"}},
         {}}};
    document.outputs = {{"absolute", 3, "value"},
                        {"minimum", 4, "value"},
                        {"maximum", 5, "value"}};
    GraphContext graph(document);
    auto plan = take(Compiler(registry).compile(graph)).plan;
    auto frozen = take(context.freeze(plan, {{{"a", a}, {"b", b}}}));
    auto query = take(Footprint::from_regions(
        {2, 3}, {Region({{0, 1}, {1, 1}}), Region({{1, 1}, {2, 1}})}));
    auto result = take(context.execute_fragments(
        frozen, {{"absolute", query}, {"minimum", query}, {"maximum", query}}));
    const std::vector<std::vector<double>> expected{{3, 6}, {2, 6}, {3, 6}};
    const std::vector<const char*> names{"absolute", "minimum", "maximum"};
    for (std::size_t i = 0; i < names.size(); ++i) {
      const auto& output = result.results.at(names[i]);
      if (take(output.descriptor()).tensor_coverage(0) != query ||
          sample(output, {0, 1}) != expected[i][0] ||
          sample(output, {1, 2}) != expected[i][1])
        throw std::runtime_error("finite sparse workflow oracle failed");
    }
    const auto support = take(result.dependencies.source_support());
    if (support.at("a") != query || support.at("b") != query)
      throw std::runtime_error("finite sparse workflow support failed");
    std::cout
        << "Result finite sparse workflow: abs=[3,6] min=[2,6] max=[3,6]\n";
    return 0;
  } catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  }
}
