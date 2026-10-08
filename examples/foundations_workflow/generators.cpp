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
void generator_gain() {
  auto registry = ps::make_default_operation_registry();
  ps::ExecutionContext execution(registry, {4, false, 32, 1024 * 1024, 65536});
  auto root = take(execution.resource_budget());
  const auto image = array<float>({-2, 3, 4, .5F}, {1, 1, 4});
  const auto coefficient = array<double>({1});
  auto doc = result_document(
      {image, coefficient},
      {{1,
        "numeric.sample_expression",
        {ps::WorkflowInputReference{2}},
        sample("c[0]*2", 1, 1)},
       {2,
        "image.exposure_gain",
        {ps::WorkflowInputReference{1}, ps::WorkflowNodeOutput{1, "value"}},
        {}}});
  auto image_schema = schema(image);
  doc.outputs.push_back({"gain", 1, "value"});
  image_schema.id = "photospider.image";
  auto& pixels = image_schema.tensors[0];
  pixels.key = "pixels";
  pixels.batch_axes = {1, 1};
  pixels.layout.spatial = true;
  pixels.layout.channel_axis = 2;
  pixels.facets = facets(ps::rgba_semantics());
  doc.inputs[0].result_schema =
      std::make_shared<ps::SchemaTemplate>(image_schema);
  ps::GraphContext graph(doc);
  auto plan = take(ps::Compiler(registry).compile(graph)).plan;
  auto bound = result_bindings(root, {image, coefficient});
  bound.inputs[0].result = source(root, image, &image_schema);
  std::map<double, ps::ResultRef> coefficients;
  for (double c : {.5, 1., 2., 3., 4., 8., 10.})
    coefficients.emplace(c, source(root, array<double>({c})));
  std::map<double, ps::FrozenExecution> frozen;
  for (const auto& entry : coefficients) {
    auto b = bound;
    b.inputs[1].result = entry.second;
    frozen.emplace(entry.first, take(execution.freeze(plan, b)));
  }
  auto run = [&](double c) { return execution.execute(frozen.at(c)); };
  std::optional<ps::ExecutionResult> retained;
  for (double c : {.5, 1., 2., 8.}) {
    auto answer = take(run(c));
    if (c == 2)
      retained = answer;
    result_exact(ps::Result<ps::ExecutionResult>(std::move(answer)),
                 {static_cast<float>(-4 * c), static_cast<float>(6 * c),
                  static_cast<float>(8 * c), .5F});
  }
  auto a = std::async(std::launch::async, [&] { return run(3); });
  auto b = std::async(std::launch::async, [&] { return run(4); });
  result_exact(a.get(), {-12, 18, 24, .5F});
  result_exact(b.get(), {-16, 24, 32, .5F});
  auto warm = take(run(2));
  require(warm.results.at("result").object_id() ==
                  retained->results.at("result").object_id() &&
              warm.results.at("gain").object_id() ==
                  retained->results.at("gain").object_id() &&
              warm.diagnostics.operation_timings.empty(),
          "retained warm Result was not reused in its frozen scope");
  auto invalid = run(10);
  require(!invalid.ok(), "out-of-range generated gain succeeded");
  auto producer_doc = doc;
  producer_doc.nodes.resize(1);
  producer_doc.outputs = {{"result", 1, "value"}};
  ps::GraphContext producer_graph(producer_doc);
  auto producer_plan =
      take(ps::Compiler(registry).compile(producer_graph)).plan;
  auto bad = bound;
  bad.inputs[1].result = coefficients.at(10);
  auto producer = take(execution.execute(producer_plan, bad));
  result_exact(ps::Result<ps::ExecutionResult>(producer), {20});
  require(!execution.execute(plan, bad).ok(),
          "cached out-of-range gain succeeded");
  bad.inputs[1].result =
      source(root, array<double>({std::numeric_limits<double>::quiet_NaN()}));
  rejected(execution.execute(plan, bad), ps::ErrorCode::OperationFailed);
  std::cout
      << "generator-gain plan_reused=passed sequential_and_concurrent=passed "
         "fresh_and_cached_invalid=rejected shared_reuse=passed "
         "producer_timings="
      << warm.diagnostics.operation_timings.size() << " oracle=passed\n";
}
}  // namespace foundations
