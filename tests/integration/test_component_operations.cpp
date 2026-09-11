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
Value mask(std::vector<float> values, std::vector<std::uint64_t> shape) {
  return array(values, shape,
               {encode_semantic(coverage_semantics()).take_value()});
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
           return base->invoke(name, call);
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
int threshold_cases() {
  const auto input =
      array<float>({-3, -0.F, .499F, .5F, 1, 4}, {2, 3}, field());
  WorkflowNode t{2,
                 "mask.threshold",
                 {WorkflowNodeOutput{1, "value"}},
                 {{"threshold", .5}}};
  auto result = run(input, {t});
  PS_CHECK(result.ok() &&
           exact<float>(result.value().values.at("2"), {0, 0, 0, 1, 1, 1}));
  PS_CHECK(result.value().values.at("2").facets()[0].payload ==
           encode_semantic(coverage_semantics()).value().payload);
  t.parameters["threshold"] = -2.;
  PS_CHECK(
      exact<float>(run(input, {t}).value().values.at("2"), {0, 1, 1, 1, 1, 1}));
  t.parameters.clear();
  PS_CHECK(run(input, {t}).status().code == ErrorCode::InvalidArgument);
  t.parameters["threshold"] = std::numeric_limits<double>::quiet_NaN();
  PS_CHECK(run(input, {t}).status().code == ErrorCode::InvalidArgument);
  t.parameters["threshold"] = .5;
  PS_CHECK(run(array<float>({1}, {1, 1}), {t}).status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(run(mask({1}, {1, 1}), {t}).status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(run(array<float>({std::numeric_limits<float>::infinity()}, {1, 1},
                            field()),
               {t})
               .status()
               .code == ErrorCode::OperationFailed);
  auto chain = run(input, {t, node(3, "mask.components", 2, 1),
                           node(4, "component.count", 3, 1)});
  PS_CHECK(chain.ok() &&
           exact<std::int64_t>(chain.value().values.at("4"), {1}));
  return 0;
}
int component_cases() {
  auto all = [](std::int64_t producer, std::int64_t attributes) {
    return std::vector<WorkflowNode>{node(2, "mask.components", 1, producer),
                                     node(3, "component.count", 2, attributes),
                                     node(4, "component.area", 2, attributes),
                                     node(5, "component.bbox", 2, attributes)};
  };
  auto empty = run(mask(std::vector<float>(6), {2, 3}), all(1, 3), true);
  PS_CHECK(empty.ok());
  PS_CHECK(exact<std::int64_t>(empty.value().values.at("2"),
                               std::vector<std::int64_t>(6)));
  PS_CHECK(exact<std::int64_t>(empty.value().values.at("3"), {0}));
  PS_CHECK(exact<std::int64_t>(empty.value().values.at("4"),
                               std::vector<std::int64_t>(4)));
  PS_CHECK(exact<std::int64_t>(empty.value().values.at("5"),
                               std::vector<std::int64_t>(16)));
  const auto bridge = mask({1, 0, 1, 1, 1, 1}, {2, 3});
  auto connected = run(bridge, all(1, 2), true);
  PS_CHECK(connected.ok());
  PS_CHECK(exact<std::int64_t>(connected.value().values.at("2"),
                               {1, 0, 1, 1, 1, 1}));
  PS_CHECK(exact<std::int64_t>(connected.value().values.at("3"), {1}));
  PS_CHECK(exact<std::int64_t>(connected.value().values.at("4"), {0, 5, 0}));
  PS_CHECK(exact<std::int64_t>(connected.value().values.at("5"),
                               {0, 0, 0, 0, 0, 0, 3, 2, 0, 0, 0, 0}));
  PS_CHECK(connected.value().values.at("2").facets()[0].payload ==
           field("component_label")[0].payload);
  PS_CHECK(connected.value().values.at("3").facets().empty() &&
           connected.value().values.at("4").facets().empty() &&
           connected.value().values.at("5").facets().empty());
  const auto diagonal = mask({1, 0, 1, 0, 1, 0, 1, 0, 1}, {3, 3});
  auto five = run(diagonal, all(5, 5), true);
  PS_CHECK(five.ok() && exact<std::int64_t>(five.value().values.at("2"),
                                            {1, 0, 2, 0, 3, 0, 4, 0, 5}));
  PS_CHECK(exact<std::int64_t>(five.value().values.at("3"), {5}));
  PS_CHECK(
      exact<std::int64_t>(five.value().values.at("4"), {0, 1, 1, 1, 1, 1}));
  PS_CHECK(exact<std::int64_t>(five.value().values.at("5"),
                               {0, 0, 0, 0, 0, 0, 1, 1, 2, 0, 3, 1,
                                1, 1, 2, 2, 0, 2, 1, 3, 2, 2, 3, 3}));
  PS_CHECK(run(diagonal, all(4, 5)).status().code ==
           ErrorCode::OperationFailed);
  auto independent = run(mask({1, 0, 1}, {1, 3}), all(10, 5));
  PS_CHECK(independent.ok() &&
           exact<std::int64_t>(independent.value().values.at("3"), {2}));
  for (float bad : {.5F, std::numeric_limits<float>::denorm_min()})
    PS_CHECK(run(mask({bad}, {1, 1}), {node(2, "mask.components", 1, 1)})
                 .status()
                 .code == ErrorCode::OperationFailed);
#if defined(__APPLE__)
  const auto subnormal =
      mask({std::numeric_limits<float>::denorm_min()}, {1, 1});
  fenv_t saved;
  PS_CHECK(std::fegetenv(&saved) == 0);
  PS_CHECK(std::fesetenv(FE_DFL_DISABLE_DENORMS_ENV) == 0);
  const auto rejected =
      run(subnormal, {node(2, "mask.components", 1, 1)}).status().code;
  const auto restored = std::fesetenv(&saved);
  PS_CHECK(restored == 0 && rejected == ErrorCode::OperationFailed);
#endif
  for (std::int64_t cap : {INT64_C(0), INT64_C(-1), INT64_C(9007199254740992)})
    PS_CHECK(run(bridge, {node(2, "mask.components", 1, cap)}).status().code ==
             ErrorCode::InvalidArgument);
  return 0;
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
  const float one = 1;
  std::vector<std::uint8_t> broadcast_bytes(5);
  std::memcpy(broadcast_bytes.data() + 1, &one, 4);
  auto broadcast =
      Value::create({ElementType::Float32, {2, 3}}, Region::whole({2, 3}),
                    {1, {0, 0}}, broadcast_bytes,
                    {encode_semantic(coverage_semantics()).take_value()})
          .take_value();
  auto connected = run(broadcast, {node(2, "mask.components", 1, 1)});
  PS_CHECK(connected.ok() &&
           exact<std::int64_t>(connected.value().values.at("2"),
                               {1, 1, 1, 1, 1, 1}));
  return 0;
}
int cancellation() {
  auto registry = make_default_operation_registry();
  for (const char* key : {"mask.components", "component.count"}) {
    for (unsigned boundary : {1U, 2U}) {
      CancellationSource cancellation;
      unsigned allocations = 0;
      std::uint64_t live = 0;
      BufferAllocator allocator([&](std::uint64_t bytes) {
        live += bytes;
        if (++allocations == boundary)
          cancellation.cancel();
        return Result<std::shared_ptr<void>>(
            std::shared_ptr<void>(new int(0), [&, bytes](void* p) {
              delete static_cast<int*>(p);
              live -= bytes;
            }));
      });
      const auto value = std::string(key) == "mask.components"
                             ? mask({1, 1}, {1, 2})
                             : labels({1, 1}, {1, 2});
      const std::vector<Value> inputs{value};
      const std::vector<Region> demands{value.region()};
      const std::map<std::string, ParameterValue> parameters{
          {"capacity", INT64_C(1)}};
      const auto region = std::string(key) == "mask.components"
                              ? Region::whole({1, 2})
                              : Region::whole({1});
      auto result =
          registry->invoke(key, {inputs, demands, parameters, Backend::Cpu,
                                 cancellation.token(), region, allocator});
      PS_CHECK(result.status().code == ErrorCode::Cancelled && live == 0 &&
               allocations == boundary);
    }
  }
  CancellationSource cancelled;
  cancelled.cancel();
  PS_CHECK(run(mask({.5F}, {1, 1}), {node(2, "mask.components", 1, 1)}, false,
               {}, cancelled.token())
               .status()
               .code == ErrorCode::Cancelled);
  PS_CHECK(run(mask(std::vector<float>(1024, 1), {32, 32}),
               {node(2, "mask.components", 1, 1)}, false, {1, false, 8, 128})
               .status()
               .code == ErrorCode::ResourceExhausted);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(threshold_cases() == 0);
  PS_CHECK(component_cases() == 0);
  PS_CHECK(attribute_cases() == 0);
  PS_CHECK(views() == 0);
  PS_CHECK(cancellation() == 0);
  return 0;
}
