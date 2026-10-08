#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

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
double sample(const ps::ResultRef& result, std::uint64_t at) {
  double number = 0;
  require(result.read_tensor(take(result.descriptor()), 0, {at}, &number, 8));
  return number;
}
}  // namespace
int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  try {
    auto registry = make_default_operation_registry();
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    config.result_cache_bytes = 4096;
    ExecutionContext context(registry, config);
    auto root = take(context.resource_budget());
    SchemaTemplate schema;
    schema.id = "example.ordered.input";
    ResultTensorSpec tensor;
    tensor.key = "data";
    tensor.descriptor = {ElementType::Float64, {6}};
    schema.tensors.push_back(std::move(tensor));
    auto source = take(ResultBuilder::start(root, schema, "ordered.input"));
    require(source.bind_descriptor_relation(
        take(ResultRelation::cartesian(root, 1, {}))));
    const double data[] = {1, 2, 3, 4, 5, 6};
    require(source.publish_tensor(
        0, Region::whole({6}),
        ByteView(reinterpret_cast<const std::uint8_t*>(data), sizeof(data)),
        take(ResultRelation::cartesian(root, 6, {})),
        {true, true, true, true}));
    auto input = take(source.seal());
    WorkflowDocument document;
    WorkflowInputDeclaration declaration;
    declaration.id = 1;
    declaration.name = "x";
    declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
    document.inputs = {declaration};
    document.nodes = {{2,
                       "numeric.mean",
                       {WorkflowInputReference{1}},
                       {{"block_size", INT64_C(2)}}},
                      {3,
                       "numeric.variance",
                       {WorkflowInputReference{1}},
                       {{"block_size", INT64_C(2)}}},
                      {4,
                       "numeric.ordered_scan",
                       {WorkflowInputReference{1}},
                       {{"block_size", INT64_C(2)}}}};
    document.outputs = {{"mean", 2, "value"},
                        {"variance", 3, "value"},
                        {"scan", 4, "value"}};
    GraphContext graph(document);
    auto plan = take(Compiler(registry).compile(graph)).plan;
    auto result = take(context.execute(plan, {{{"x", input}}}));
    const auto mean = sample(result.results.at("mean"), 0);
    const auto variance = sample(result.results.at("variance"), 0);
    double carry = 0;
    for (std::uint64_t i = 0; i < 6; ++i) {
      carry += data[i];
      if (sample(result.results.at("scan"), i) != carry)
        throw std::runtime_error("ordered prefix oracle failed");
    }
    if (mean != 3.5 || std::abs(variance - 35. / 12) > 1e-15)
      throw std::runtime_error("ordered reduction oracle failed");
    std::cout << "Result ordered workflow: mean=3.5 variance=35/12 "
                 "prefixes=[1,3,6,10,15,21]\n";
    return 0;
  } catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  }
}
