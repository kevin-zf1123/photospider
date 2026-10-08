#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
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
template <class T>
ps::ResultRef source(const ps::ResourceBudget& root,
                     const ps::SchemaTemplate& schema,
                     const std::vector<T>& values) {
  auto builder = take(ps::ResultBuilder::start(root, schema, "basic.input"));
  require(builder.bind_descriptor_relation(
      take(ps::ResultRelation::cartesian(root, 1, {}))));
  require(builder.publish_tensor(
      0, ps::Region::whole(schema.tensors[0].sample_shape()),
      ps::ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                   values.size() * sizeof(T)),
      take(ps::ResultRelation::cartesian(root, values.size(), {})),
      {true, true, true, true}));
  return take(builder.seal());
}
ps::SchemaTemplate numeric(ps::ElementType type,
                           std::vector<std::uint64_t> shape) {
  ps::SchemaTemplate schema;
  schema.id = "example.basic.input";
  ps::ResultTensorSpec tensor;
  tensor.key = "data";
  tensor.descriptor = {type, std::move(shape)};
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
template <class T>
T sample(const ps::ResultRef& result, const std::vector<std::uint64_t>& at) {
  T value;
  require(result.read_tensor(take(result.descriptor()), 0, at, &value,
                             sizeof(value)));
  return value;
}
}  // namespace
int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  try {
    auto registry = make_default_operation_registry();
    ExecutionContext context(registry);
    auto root = take(context.resource_budget());
    const auto field_schema = numeric(ElementType::Float32, {2, 2});
    const auto table_schema = numeric(ElementType::Float32, {2});
    const auto controls_schema = numeric(ElementType::Float64, {3, 2});
    auto rgba_schema = numeric(ElementType::Float32, {2, 2, 4});
    rgba_schema.id = "photospider.image";
    auto& rgba = rgba_schema.tensors[0];
    rgba.key = "pixels";
    rgba.batch_axes = {1, 1};
    rgba.layout.spatial = true;
    rgba.facets = {take(encode_semantic(rgba_semantics()))};
    WorkflowDocument document;
    ExecutionBindings bindings;
    auto bind = [&](std::uint64_t id, const char* name,
                    const SchemaTemplate& schema, ResultRef object) {
      WorkflowInputDeclaration declaration;
      declaration.id = id;
      declaration.name = name;
      declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
      document.inputs.push_back(std::move(declaration));
      bindings.inputs.push_back({name, std::move(object)});
    };
    bind(1, "field", field_schema,
         source<float>(root, field_schema, {-1, 0, .5F, 2}));
    bind(2, "table", table_schema, source<float>(root, table_schema, {0, 2}));
    bind(3, "controls", controls_schema,
         source<double>(root, controls_schema, {0, 0, .5, .25, 1, 1}));
    bind(4, "rgba", rgba_schema,
         source<float>(root, rgba_schema,
                       {1, .5F, .25F, 1, 1, .5F, .25F, 1, 1, .5F, .25F, 1, 1,
                        .5F, .25F, 1}));
    const std::map<std::string, ParameterValue> domain = {
        {"domain_min", 0.},
        {"domain_max", 1.},
        {"out_of_domain", std::string("reject")}};
    auto curve_parameters = domain;
    curve_parameters["count"] = INT64_C(5);
    document.nodes = {
        {10,
         "field.smoothstep",
         {WorkflowInputReference{1}},
         {{"edge0", 0.}, {"edge1", 1.}}},
        {11,
         "field.apply_lut_1d",
         {WorkflowNodeOutput{10, "value"}, WorkflowInputReference{2}},
         domain},
        {12,
         "grade.levels",
         {WorkflowNodeOutput{11, "value"}},
         {{"black", 0.},
          {"white", 2.},
          {"gamma", 1.},
          {"out_min", 0.},
          {"out_max", 1.}}},
        {13,
         "analysis.histogram",
         {WorkflowNodeOutput{12, "value"}},
         {{"range_min", 0.}, {"range_max", 1.}, {"bins", INT64_C(4)}}},
        {14,
         "analysis.histogram_out_of_range",
         {WorkflowNodeOutput{12, "value"}},
         {{"range_min", 0.}, {"range_max", 1.}}},
        {15,
         "curve.sample_linear",
         {WorkflowInputReference{3}},
         curve_parameters},
        {16,
         "curve.sample_monotone",
         {WorkflowInputReference{3}},
         curve_parameters},
        {17,
         "image.mask",
         {WorkflowInputReference{4}, WorkflowNodeOutput{10, "value"}},
         {}}};
    document.outputs = {{"coverage", 10, "value"}, {"levels", 12, "value"},
                        {"bins", 13, "value"},     {"outside", 14, "value"},
                        {"linear", 15, "value"},   {"monotone", 16, "value"},
                        {"masked", 17, "value"}};
    GraphContext graph(document);
    auto compiled = take(Compiler(registry).compile(graph));
    auto result = take(context.execute(compiled.plan, bindings));
    const float coverage[] = {0, 0, .5F, 1};
    const std::int64_t counts[] = {2, 0, 1, 1};
    const double linear[] = {0, .125, .25, .625, 1};
    const double monotone[] = {0, .078125, .25, .546875, 1};
    for (std::uint64_t i = 0; i < 4; ++i) {
      if (sample<float>(result.results.at("coverage"), {0, 0, i / 2, i % 2}) !=
              coverage[i] ||
          sample<float>(result.results.at("levels"), {0, 0, i / 2, i % 2}) !=
              coverage[i] ||
          sample<float>(result.results.at("masked"), {0, 0, i / 2, i % 2, 3}) !=
              coverage[i] ||
          sample<std::int64_t>(result.results.at("bins"), {i}) != counts[i])
        throw std::runtime_error("basic composed workflow oracle failed");
    }
    for (std::uint64_t i = 0; i < 5; ++i)
      if (sample<double>(result.results.at("linear"), {i}) != linear[i] ||
          sample<double>(result.results.at("monotone"), {i}) != monotone[i])
        throw std::runtime_error("curve oracle failed");
    for (std::uint64_t i = 0; i < 2; ++i)
      if (sample<std::int64_t>(result.results.at("outside"), {i}) != 0)
        throw std::runtime_error("out-of-range oracle failed");
    std::cout << "coverage=[0,0,.5,1] bins=[2,0,1,1] outside=[0,0] "
                 "linear/PCHIP/masked_alpha=passed backend=cpu\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
