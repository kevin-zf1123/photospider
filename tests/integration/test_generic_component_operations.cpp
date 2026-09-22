#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
std::vector<ValueFacet> field(const std::string& role = "value") {
  SemanticDescriptor s;
  s.kind = SemanticKind::ScalarField;
  s.channels = {{role, role, "dimensionless"}};
  return {encode_semantic(s).take_value()};
}
template <class T>
Value array(const std::vector<T>& samples, std::vector<std::uint64_t> shape,
            std::vector<ValueFacet> facets = {}) {
  auto made = MutableValue::allocate(
      {std::is_same_v<T, float> ? ElementType::Float32 : ElementType::Int64,
       shape},
      Region::whole(shape), BufferAllocator{});
  auto value = made.take_value();
  std::memcpy(value.data(), samples.data(), samples.size() * sizeof(T));
  return std::move(value).publish(facets).take_value();
}
Value labels(std::vector<std::int64_t> values,
             std::vector<std::uint64_t> shape) {
  return array(values, shape, field("component_label"));
}
template <class T>
bool exact(const Value& value, const std::vector<T>& expected) {
  return value.bytes().size() == expected.size() * sizeof(T) &&
         std::memcmp(value.bytes().data(), expected.data(),
                     expected.size() * sizeof(T)) == 0;
}
WorkflowNode node(std::uint64_t id, const std::string& key, std::uint64_t input,
                  std::int64_t capacity) {
  return {id,
          key,
          {WorkflowNodeOutput{input, "value"}},
          {{"capacity", capacity}}};
}
Result<ExecutionResult> run(const Value& input, std::vector<WorkflowNode> nodes,
                            bool tiled = false,
                            ExecutionContextConfig config = {},
                            CancellationToken cancellation = {}) {
  // Public immutable producer permits every legal strided/origin/broadcast
  // view.
  auto base = make_default_operation_registry();
  auto registry = std::make_shared<OperationRegistry>();
  OperationTraits source;
  source.outputs[0].output_element_type = input.descriptor().element_type;
  source.estimated_bytes = input.storage()->capacity();
  source.outputs[0].shape_rule = OperationShapeRule::Fixed;
  source.outputs[0].fixed_output_shape = input.descriptor().shape;
  source.outputs[0].output_semantic_rule = OperationSemanticRule::Establish;
  source.outputs[0].output_facets = input.facets();
  auto status = registry->register_operation(
      {"fixture.input", source,
       [input](const OperationInvocation&) { return Result<Value>(input); }});
  if (!status.ok())
    return Result<ExecutionResult>(status);
  for (const char* key :
       {"mask.threshold", "mask.components", "component.count",
        "component.area", "component.bbox"}) {
    status = registry->register_operation(
        {key, base->find_traits(key).take_value(),
         [base, name = std::string(key)](const OperationInvocation& call) {
           auto forwarded = call;
           forwarded.prepared.reset();
           return base->invoke(name, forwarded);
         }});
    if (!status.ok())
      return Result<ExecutionResult>(status);
  }
  registry->freeze();
  WorkflowDocument doc;
  doc.nodes = {{1, "fixture.input", {}, {}}};
  doc.nodes.insert(doc.nodes.end(), nodes.begin(), nodes.end());
  for (const auto& n : nodes)
    doc.outputs.push_back({std::to_string(n.id), n.id, "value"});
  GraphContext graph(doc);
  PlanningOptions options;
  if (tiled) {
    options.tile_height = 1;
    options.tile_width = 1;
  }
  auto compiled = Compiler(registry).compile(graph, options);
  if (!compiled.ok())
    return Result<ExecutionResult>(compiled.status());
  ExecutionContext execution(registry, config);
  return execution.execute(compiled.value().plan, {}, cancellation);
}
int attribute_cases() {
  const auto sparse = labels({0, 2, 2, 5, 0, 5}, {2, 3});
  auto result = run(sparse, {node(2, "component.count", 1, 5),
                             node(3, "component.area", 1, 5),
                             node(4, "component.bbox", 1, 5)});
  PS_CHECK(result.ok());
  PS_CHECK(exact<std::int64_t>(result.value().values.at("2"), {2}));
  PS_CHECK(
      exact<std::int64_t>(result.value().values.at("3"), {0, 0, 2, 0, 0, 2}));
  PS_CHECK(exact<std::int64_t>(result.value().values.at("4"),
                               {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 3, 1,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 2}));
  for (const char* key :
       {"component.count", "component.area", "component.bbox"}) {
    PS_CHECK(run(sparse, {node(2, key, 1, 4)}).status().code ==
             ErrorCode::OperationFailed);
    for (auto bad : {INT64_C(-1), INT64_MAX})
      PS_CHECK(run(labels({bad}, {1, 1}), {node(2, key, 1, 5)}).status().code ==
               ErrorCode::OperationFailed);
    PS_CHECK(
        run(array<std::int64_t>({1}, {1, 1}, field()), {node(2, key, 1, 1)})
            .status()
            .code == ErrorCode::TypeMismatch);
  }
  // Large capacity must not drive count allocation; sparse IDs remain exact.
  auto large = run(labels({9007199254740991LL, 1, 9007199254740991LL}, {1, 3}),
                   {node(2, "component.count", 1, 9007199254740991LL)}, false,
                   {1, false, 8, 4096});
  PS_CHECK(large.ok() &&
           exact<std::int64_t>(large.value().values.at("2"), {2}));
  auto too_large =
      run(sparse, {node(2, "component.bbox", 1, 9007199254740991LL)}, false,
          {1, false, 8, 4096});
  PS_CHECK(too_large.status().code == ErrorCode::ResourceExhausted);
  return 0;
}
int views() {
  std::vector<std::uint8_t> bytes(32);
  const std::int64_t data[] = {2, 0, 5};
  std::memcpy(bytes.data() + 1, data, 24);
  auto reversed =
      Value::create({ElementType::Int64, {1, 3}}, Region::whole({1, 3}),
                    {1, {0, -8}, {0, 2}}, bytes, field("component_label"))
          .take_value();
  auto result = run(reversed, {node(2, "component.bbox", 1, 5),
                               node(3, "component.area", 1, 5)});
  if (!result.ok())
    std::cerr << result.status().message << "\n";
  PS_CHECK(result.ok() && exact<std::int64_t>(result.value().values.at("3"),
                                              {0, 0, 1, 0, 0, 1}));
  PS_CHECK(exact<std::int64_t>(result.value().values.at("2"),
                               {0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 3, 1,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1}));
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(attribute_cases() == 0);
  PS_CHECK(views() == 0);
  return 0;
}
