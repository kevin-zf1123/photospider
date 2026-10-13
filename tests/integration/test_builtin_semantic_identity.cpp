#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "photospider/data/semantic.hpp"
#include "photospider/ops/registry.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"
namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
SemanticDescriptor signal() {
  SemanticDescriptor s;
  s.kind = SemanticKind::SampledSignal;
  s.channels = {{"value", "value", "dimensionless"}};
  s.sample_origin = 0;
  s.sample_step = .5;
  s.sample_axis_unit = "seconds";
  return s;
}
SchemaTemplate semantic_schema(const SemanticDescriptor& semantic) {
  const bool image = semantic.kind == SemanticKind::Image;
  const bool mask = semantic.kind == SemanticKind::Mask;
  auto schema = multi_result::schema(ElementType::Float32,
                                     image ? std::vector<std::uint64_t>{1, 1, 4}
                                     : mask ? std::vector<std::uint64_t>{1, 1}
                                            : std::vector<std::uint64_t>{3});
  schema.tensors[0].facets = {take(encode_semantic(semantic))};
  schema.tensors[0].layout.spatial = image;
  return schema;
}
ResultRef source(const ResourceBudget& root, const SchemaTemplate& schema,
                 const std::vector<float>& values) {
  auto builder = take(ResultBuilder::start(root, schema, "semantic.source"));

  const auto& tensor = schema.tensors[0];
  check(builder.publish_tensor(
      0, Region::whole(tensor.sample_shape()),
      ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
               values.size() * sizeof(float))));
  return take(builder.seal());
}
int builtin_identity() {
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext execution(registry, config);
  auto bytes = Value::create({ElementType::UInt8, {2}}, Region::whole({2}),
                             {0, {1}}, {3, 7}, {{"opaque", 1, {9}}})
                   .take_value();
  auto root = take(execution.resource_budget());
  auto byte_schema = multi_result::schema(ElementType::UInt8, {2});
  byte_schema.tensors[0].facets = bytes.facets();
  auto builder =
      take(ResultBuilder::start(root, byte_schema, "identity.source"));

  check(builder.publish_tensor(0, bytes.region(), bytes.layout(),
                               bytes.storage()));
  const std::vector<ResultRef> inputs{
      take(builder.seal()),
      source(root, semantic_schema(rgba_semantics()), {-2, 4, .25F, .5F}),
      source(root, semantic_schema(coverage_semantics()), {.5F}),
      source(root, semantic_schema(signal()), {1, 2, 3})};
  for (const auto& input : inputs) {
    for (const auto* key : {"core.identity"}) {
      WorkflowDocument doc;
      doc.inputs = {multi_result::declaration(1, "input", input.schema())};
      std::map<std::string, ParameterValue> parameters;
      doc.nodes = {{1, key, {WorkflowInputReference{1}}, parameters}};
      doc.outputs = {{"output", 1, "value"}};
      GraphContext graph(doc);
      auto compiled = compiler.compile(graph);
      PS_CHECK(compiled.ok());
      auto run = execution.execute(compiled.value().plan, {{{"input", input}}});
      PS_CHECK(run.ok());
      const auto& output = run.value().results.at("output");
      PS_CHECK(output.schema().tensors[0].descriptor.element_type ==
               input.schema().tensors[0].descriptor.element_type);
      PS_CHECK(output.schema().tensors[0].facets[0].payload ==
               input.schema().tensors[0].facets[0].payload);
      const auto shape = input.schema().tensors[0].sample_shape();
      auto coverage = take(Footprint::all(shape));
      const auto input_facts = take(input.descriptor());
      const auto output_facts = take(output.descriptor());
      const auto width = Value::element_size(
          input.schema().tensors[0].descriptor.element_type);
      auto compared = coverage.visit(
          [&](const auto& at) {
            std::uint8_t expected[8]{}, actual[8]{};
            auto status =
                input.read_tensor(input_facts, 0, at, expected, width);
            if (status.ok())
              status = output.read_tensor(output_facts, 0, at, actual, width);
            if (!status.ok())
              return status;
            return std::memcmp(expected, actual, width) == 0
                       ? Status::success()
                       : Status{ErrorCode::OperationFailed,
                                "identity changed sample bits"};
          },
          take(coverage.element_count()));
      PS_CHECK(compared.ok());
    }
  }
  WorkflowDocument bad;
  WorkflowInputDeclaration bad_input;
  bad_input.id = 1;
  bad_input.name = "input";
  SchemaTemplate bad_schema;
  bad_schema.id = "photospider.tensor";
  ResultTensorSpec bad_tensor;
  bad_tensor.key = "samples";
  bad_tensor.descriptor = bytes.descriptor();
  bad_tensor.facets = bytes.facets();
  bad_schema.tensors.push_back(bad_tensor);
  bad_input.result_schema = std::make_shared<SchemaTemplate>(bad_schema);
  bad.inputs = {bad_input};
  bad.nodes = {{1,
                "math.add",
                {WorkflowInputReference{1}, WorkflowInputReference{1}},
                {}}};
  bad.outputs = {{"output", 1, "value"}};
  GraphContext bad_graph(bad);
  PS_CHECK(compiler.compile(bad_graph).status().code ==
           ErrorCode::TypeMismatch);
  return 0;
}
}  // namespace
int main() {
  return builtin_identity();
}
