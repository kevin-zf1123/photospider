#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Parameters = std::map<std::string, ParameterValue>;
Value value(const std::vector<std::uint64_t>& shape,
            const std::vector<float>& samples,
            const std::vector<ValueFacet>& facets = {}) {
  auto made = MutableValue::allocate({ElementType::Float32, shape},
                                     Region::whole(shape), BufferAllocator{});
  auto output = made.take_value();
  std::memcpy(output.data(), samples.data(), samples.size() * 4);
  return std::move(output).publish(facets).take_value();
}
SemanticDescriptor descriptor(const Value& v) {
  return decode_semantic(v.facets()[0]).take_value();
}
Value output(const Result<ExecutionResult>& r) {
  return r.value().values.at("result");
}
bool close(const Value& actual, const std::vector<float>& expected,
           double tolerance = 1e-5) {
  if (actual.bytes().size() != expected.size() * 4)
    return false;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    float number;
    std::memcpy(&number, actual.bytes().data() + i * 4, 4);
    if (!std::isfinite(number) || std::abs(number - expected[i]) >
                                      tolerance * (1 + std::abs(expected[i])))
      return false;
  }
  return true;
}
Parameters selection(std::initializer_list<std::uint32_t> indices) {
  return {{"indices", channel_indices_parameter(indices).take_value()}};
}
Result<ExecutionResult> graph(const std::vector<Value>& inputs,
                              const std::vector<WorkflowNode>& nodes,
                              std::shared_ptr<OperationRegistry> registry = {},
                              ExecutionContextConfig config = {}) {
  if (!registry)
    registry = make_default_operation_registry();
  WorkflowDocument doc;
  ExecutionBindings bindings;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto name = "input" + std::to_string(i);
    doc.inputs.push_back({i + 1, name, inputs[i].descriptor(),
                          inputs[i].region(), inputs[i].layout(),
                          inputs[i].facets()});
    bindings.inputs.push_back({name, inputs[i]});
  }
  doc.nodes = nodes;
  doc.outputs = {{"result", nodes.back().id, "value"}};
  GraphContext context(doc);
  Compiler compiler(registry);
  auto plan = compiler.compile(context);
  if (!plan.ok())
    return Result<ExecutionResult>(plan.status());
  ExecutionContext execution(registry, config);
  return execution.execute(plan.value().plan, bindings);
}
Result<ExecutionResult> run(const std::string& key,
                            const std::vector<Value>& inputs,
                            const Parameters& parameters = {}) {
  std::vector<WorkflowInput> references;
  for (std::size_t i = 0; i < inputs.size(); ++i)
    references.push_back(WorkflowInputReference{i + 1});
  return graph(inputs, {{1, key, references, parameters}});
}
int generic_fields() {
  // A target can establish non-image HWC channels through the same merge rule.
  SemanticDescriptor vector;
  vector.kind = SemanticKind::VectorField;
  vector.unit = "pixels";
  vector.channels = {{"dx", "x", "pixels"}, {"dy", "y", "pixels"}};
  vector.coordinate_space = "pixel_displacement";
  vector.direction = "forward";
  auto merged = run("channel.merge", {value({1, 1}, {-2}), value({1, 1}, {3})},
                    {{"semantic", semantic_parameter(vector).take_value()}});
  PS_CHECK(merged.ok() &&
           descriptor(output(merged)).kind == SemanticKind::VectorField &&
           close(output(merged), {-2, 3}, 0));
  auto extracted =
      run("channel.extract", {output(merged)}, {{"index", INT64_C(1)}});
  PS_CHECK(extracted.ok() && descriptor(output(extracted)).unit == "pixels");
  auto double_field = [](double sample) {
    return Value::create({ElementType::Float64, {1, 1}}, Region::whole({1, 1}),
                         {0, {8, 8}}, Value::from_float64(sample).copy_bytes())
        .take_value();
  };
  auto double_vector =
      run("channel.merge", {double_field(-2), double_field(3)},
          {{"semantic", semantic_parameter(vector).take_value()}});
  PS_CHECK(double_vector.ok() &&
           output(double_vector).descriptor().element_type ==
               ElementType::Float64);
  auto double_component =
      run("channel.extract", {output(double_vector)}, {{"index", INT64_C(1)}});
  PS_CHECK(double_component.ok());
  double component;
  std::memcpy(&component, output(double_component).bytes().data(), 8);
  PS_CHECK(component == 3 &&
           descriptor(output(double_component)).unit == "pixels");
  SemanticDescriptor complex;
  complex.kind = SemanticKind::ComplexField;
  complex.channels = {{"real", "real", "dimensionless"},
                      {"imag", "imaginary", "dimensionless"}};
  complex.coordinate_space = "frequency_unshifted";
  complex.direction = "forward_negative_inverse_1n";
  auto complex_value =
      run("channel.merge", {value({1, 1}, {-2}), value({1, 1}, {3})},
          {{"semantic", semantic_parameter(complex).take_value()}});
  PS_CHECK(complex_value.ok() && descriptor(output(complex_value)).kind ==
                                     SemanticKind::ComplexField);
  auto reversed_complex =
      run("channel.swizzle", {output(complex_value)}, selection({1, 0}));
  PS_CHECK(reversed_complex.ok() && output(reversed_complex).facets().empty() &&
           close(output(reversed_complex), {3, -2}, 0));
  return 0;
}
}  // namespace
int main() {
  return generic_fields();
}
