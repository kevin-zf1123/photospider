#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "workflow.hpp"  // NOLINT(build/include_subdir)

namespace foundations {
namespace {
ps::ResultRef numeric_result(const std::string& key,
                             const std::vector<ps::Value>& storage) {
  auto registry = ps::make_default_operation_registry();
  ps::ExecutionContextConfig config;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(registry, config);
  const auto root = take(execution.resource_budget());
  ps::WorkflowDocument doc;
  ps::ExecutionBindings bindings;
  std::vector<ps::WorkflowInput> refs;
  for (std::size_t i = 0; i < storage.size(); ++i) {
    ps::SchemaTemplate schema;
    schema.id = "example.foundations.numeric";
    ps::ResultTensorSpec tensor;
    tensor.key = "samples";
    tensor.descriptor = storage[i].descriptor();
    schema.tensors.push_back(std::move(tensor));
    auto builder =
        take(ps::ResultBuilder::start(root, schema, "numeric.input"));
    require(builder
                .bind_descriptor_relation(
                    take(ps::ResultRelation::cartesian(root, 1, {})))
                .ok(),
            "numeric source descriptor");
    require(builder
                .publish_tensor(
                    0, storage[i].region(), storage[i].bytes(),
                    take(ps::ResultRelation::cartesian(
                        root, take(schema.tensors[0].sample_count()), {})),
                    {true, true, true, true})
                .ok(),
            "numeric source publication");
    ps::WorkflowInputDeclaration input;
    input.id = i + 1;
    input.name = "input" + std::to_string(i);
    input.result_schema =
        std::make_shared<ps::SchemaTemplate>(std::move(schema));
    refs.push_back(ps::WorkflowInputReference{input.id});
    bindings.inputs.push_back({input.name, take(builder.seal())});
    doc.inputs.push_back(std::move(input));
  }
  doc.nodes = {{1, key, refs, {}}};
  doc.outputs = {{"result", 1, "value"}};
  ps::GraphContext graph(doc);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  return take(execution.execute(compiled.plan, bindings)).results.at("result");
}
template <class Number>
void numeric_exact(const ps::ResultRef& result,
                   const std::vector<Number>& expected) {
  auto descriptor = take(result.descriptor());
  require(
      take(descriptor.tensor_coverage(0).element_count()) == expected.size(),
      "numeric Result sample count");
  for (std::uint64_t i = 0; i < expected.size(); ++i) {
    Number number;
    require(
        result.read_tensor(descriptor, 0, {i}, &number, sizeof(number)).ok() &&
            std::memcmp(&number, &expected[i], sizeof(number)) == 0,
        "numeric Result bit oracle");
  }
}
}  // namespace
void numeric() {
  numeric_exact<float>(
      numeric_result("numeric.subtract",
                     {array<float>({3, 2, 1}), array<float>({4, 4, 4})}),
      {-1, -2, -3});
  numeric_exact<double>(
      numeric_result("numeric.mean", {array<float>({1, 2, 3})}), {2});
  auto variance = numeric_result("numeric.variance", {array<float>({1, 2, 3})});
  double v;
  require(variance.read_tensor(take(variance.descriptor()), 0, {0}, &v, 8).ok(),
          "variance Result read");
  require(std::abs(v - 2. / 3) < 1e-15, "variance oracle");
  std::cout << "numeric negative_ramp=[-1,-2,-3] mean=2 variance=2/3 "
               "oracle=passed\n";
}
}  // namespace foundations
