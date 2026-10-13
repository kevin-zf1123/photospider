#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "workflow.hpp"  // NOLINT(build/include_subdir)

namespace foundations {
namespace {
Parameters sample(const std::string& expression, std::int64_t count = 3,
                  double step = .5) {
  return {{"expression", expression},
          {"count", count},
          {"start", 0.},
          {"step", step}};
}
ps::Value signal(const std::vector<float>& values) {
  ps::SemanticDescriptor s;
  s.kind = ps::SemanticKind::SampledSignal;
  s.channels = {{"value", "value", "dimensionless"}};
  s.sample_step = 1;
  s.sample_axis_unit = "dimensionless";
  return array(values, {}, facets(s));
}
ps::SchemaTemplate schema(const ps::Value& storage) {
  ps::SchemaTemplate result;
  result.id = "example.foundations.input";
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = storage.descriptor();
  tensor.facets = storage.facets();
  for (const auto& facet : tensor.facets) {
    auto meaning = ps::decode_semantic(facet);
    if (meaning.ok() && meaning.value().kind == ps::SemanticKind::Image) {
      result.id = "photospider.image";
      tensor.key = "pixels";
      tensor.batch_axes = {1, 1};
      tensor.layout.spatial = true;
      tensor.layout.height_axis = 0;
      tensor.layout.width_axis = 1;
      tensor.layout.channel_axis = 2;
    }
  }
  result.tensors.push_back(std::move(tensor));
  return result;
}
ps::ResultRef source(const ps::ResourceBudget& root, const ps::Value& storage,
                     const ps::SchemaTemplate* override_schema = nullptr) {
  auto s = override_schema ? *override_schema : schema(storage);
  const auto shape = s.tensors[0].sample_shape();
  auto builder = take(ps::ResultBuilder::start(root, s, "foundations.input"));
  require(builder
              .bind_descriptor_relation(
                  take(ps::ResultRelation::cartesian(root, 1, {})))
              .ok(),
          "source descriptor");
  require(builder
              .publish_tensor(0, ps::Region::whole(shape), storage.bytes(),
                              take(ps::ResultRelation::cartesian(
                                  root, take(s.tensors[0].sample_count()), {})),
                              {true, true, true, true})
              .ok(),
          "source tensor");
  return take(builder.seal());
}
ps::WorkflowDocument result_document(
    const std::vector<ps::Value>& storage,
    const std::vector<ps::WorkflowNode>& nodes) {
  ps::WorkflowDocument doc;
  for (std::size_t i = 0; i < storage.size(); ++i) {
    ps::WorkflowInputDeclaration input;
    input.id = i + 1;
    input.name = "input" + std::to_string(i);
    input.result_schema =
        std::make_shared<ps::SchemaTemplate>(schema(storage[i]));
    doc.inputs.push_back(std::move(input));
  }
  doc.nodes = nodes;
  doc.outputs = {{"result", nodes.back().id, "value"}};
  return doc;
}
ps::ExecutionBindings result_bindings(const ps::ResourceBudget& root,
                                      const std::vector<ps::Value>& storage) {
  ps::ExecutionBindings bindings;
  for (std::size_t i = 0; i < storage.size(); ++i)
    bindings.inputs.push_back(
        {"input" + std::to_string(i), source(root, storage[i])});
  return bindings;
}
ps::Result<ps::ExecutionResult> result_run(
    const std::vector<ps::Value>& storage,
    const std::vector<ps::WorkflowNode>& nodes) {
  auto registry = ps::make_default_operation_registry();
  ps::GraphContext graph(result_document(storage, nodes));
  auto compiled = ps::Compiler(registry).compile(graph);
  if (!compiled.ok())
    return ps::Result<ps::ExecutionResult>(compiled.status());
  ps::ExecutionContext execution(registry);
  return execution.execute(
      compiled.value().plan,
      result_bindings(take(execution.resource_budget()), storage));
}
ps::Result<ps::ExecutionResult> result_operation(const std::string& key,
                                                 const ps::Value& storage,
                                                 const Parameters& params) {
  return result_run({storage},
                    {{1, key, {ps::WorkflowInputReference{1}}, params}});
}
void result_exact(ps::Result<ps::ExecutionResult> answer,
                  const std::vector<float>& expected) {
  auto execution = take(std::move(answer));
  const auto& result = execution.results.at("result");
  auto facts = take(result.descriptor());
  const auto shape = result.schema().tensors[0].sample_shape();
  require(take(facts.tensor_coverage(0).element_count()) == expected.size(),
          "Result sample count");
  std::vector<std::uint64_t> at(shape.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    float number;
    require(result.read_tensor(facts, 0, at, &number, 4).ok() &&
                std::memcmp(&number, &expected[i], 4) == 0,
            "Result sample bit oracle");
    for (std::size_t axis = shape.size(); axis-- > 0;)
      if (++at[axis] < shape[axis])
        break;
      else
        at[axis] = 0;
  }
}
}  // namespace
void expressions() {
  auto coefficients = array<double>({1});
  result_exact(result_operation("numeric.sample_expression", coefficients,
                                sample("c[0]*x^2")),
               {0, .25F, 1});
  result_exact(result_operation("numeric.sample_expression", coefficients,
                                sample("2*x+1", 5, .25)),
               {1, 1.5F, 2, 2.5F, 3});
  result_exact(
      result_run(
          {coefficients, signal({.25F})},
          {{1,
            "numeric.sample_expression",
            {ps::WorkflowInputReference{1}},
            sample("x^2")},
           {2,
            "lut.apply_1d",
            {ps::WorkflowInputReference{2}, ps::WorkflowNodeOutput{1, "value"}},
            {{"out_of_domain", std::string("reject")}}}}),
      {.125F});
  rejected(result_operation("numeric.sample_expression", coefficients,
                            sample("min(1e300*1e300,0)")),
           ps::ErrorCode::OperationFailed);
  rejected(result_operation("numeric.sample_expression", coefficients,
                            sample("c[1]")),
           ps::ErrorCode::InvalidArgument);
  std::cout << "expression-lut x_squared=[0,.25,1] query_.25=.125 "
               "count5=[1,1.5,2,2.5,3] invalid_AST_and_nonfinite=rejected "
               "oracle=passed\n";
}
}  // namespace foundations
