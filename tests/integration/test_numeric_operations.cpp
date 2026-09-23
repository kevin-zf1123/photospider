#include <atomic>
#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
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
using Parameters = std::map<std::string, ParameterValue>;
template <class T>
ElementType type() {
  return std::is_same_v<T, float>          ? ElementType::Float32
         : std::is_same_v<T, double>       ? ElementType::Float64
         : std::is_same_v<T, std::int64_t> ? ElementType::Int64
                                           : ElementType::UInt8;
}
template <class T>
Value array(const std::vector<T>& numbers) {
  std::vector<std::uint8_t> bytes(numbers.size() * sizeof(T));
  std::memcpy(bytes.data(), numbers.data(), bytes.size());
  return Value::create({type<T>(), {numbers.size()}},
                       Region::whole({numbers.size()}), {0, {sizeof(T)}}, bytes)
      .take_value();
}
/** @brief Public producer -> numeric workflow; producer allows legal strided
 * Values. */
Result<ExecutionResult> run(const std::string& operation,
                            const std::vector<Value>& inputs,
                            const Parameters& parameters = {},
                            unsigned* calls = nullptr,
                            ExecutionContextConfig config = {}) {
  auto base = make_default_operation_registry();
  auto registry = std::make_shared<OperationRegistry>();
  auto traits = base->find_traits(operation).take_value();
  if (traits.outputs[0].dependency_version) {
    // This fixture is a synchronous outer wrapper around the real public
    // direct staged invocation. Reserve its live state and scalar assembly.
    traits.workspace_bytes += traits.outputs[0].continuation_bytes + 16;
    traits.outputs[0].dependency_version = 0;
    traits.outputs[0].continuation_bytes = 0;
    traits.outputs[0].maximum_dependency_stages = 0;
    traits.outputs[0].region_rule = OperationRegionRule::Whole;
  }
  auto registered = registry->register_operation(
      {operation, traits,
       [base, operation, calls](const OperationInvocation& call) {
         if (calls)
           ++*calls;
         auto forwarded = call;
         forwarded.prepared.reset();
         return base->invoke(operation, forwarded);
       }});
  if (!registered.ok())
    return Result<ExecutionResult>(registered);
  WorkflowDocument document;
  std::vector<WorkflowInput> references;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    OperationTraits source;
    source.outputs[0].output_element_type = inputs[i].descriptor().element_type;
    source.outputs[0].shape_rule = OperationShapeRule::Fixed;
    source.outputs[0].fixed_output_shape = inputs[i].descriptor().shape;
    source.estimated_bytes = inputs[i].storage()->capacity();
    source.outputs[0].output_semantic_rule = OperationSemanticRule::Establish;
    source.outputs[0].output_facets = inputs[i].facets();
    const auto key = "fixture.input" + std::to_string(i);
    auto status = registry->register_operation(
        {key, source, [value = inputs[i]](const OperationInvocation&) {
           return Result<Value>(value);
         }});
    if (!status.ok())
      return Result<ExecutionResult>(status);
    document.nodes.push_back({i + 1, key, {}, {}});
    references.push_back(WorkflowNodeOutput{i + 1, "value"});
  }
  registry->freeze();
  document.nodes.push_back({100, operation, references, parameters});
  document.outputs = {{"result", 100, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  if (!compiled.ok())
    return Result<ExecutionResult>(compiled.status());
  ExecutionContext execution(registry, config);
  return execution.execute(compiled.value().plan);
}
Value output(const Result<ExecutionResult>& result) {
  return result.value().values.at("result");
}
template <class T>
bool equal(const Result<ExecutionResult>& result,
           const std::vector<T>& expected) {
  if (!result.ok())
    return false;
  const auto value = output(result);
  return value.descriptor().element_type == type<T>() &&
         value.facets().empty() &&
         value.bytes().size() == expected.size() * sizeof(T) &&
         std::memcmp(value.bytes().data(), expected.data(),
                     value.bytes().size()) == 0;
}
int math_and_views() {
  PS_CHECK(equal<float>(run("numeric.subtract",
                            {array<float>({3, 2, 1}), array<float>({4, 4, 4})}),
                        {-1, -2, -3}));
  PS_CHECK(equal<double>(
      run("numeric.add", {array<double>({-3, 2, 1}), array<double>({4, 4, 4})}),
      {1, 6, 5}));
  PS_CHECK(equal<float>(run("numeric.divide", {array<float>({-3, 2, 1}),
                                               array<float>({2, 4, .5F})}),
                        {-1.5F, .5F, 2}));
  PS_CHECK(equal<float>(run("numeric.clamp", {array<float>({-2, .5F, 4})},
                            {{"min", 0.}, {"max", 1.}}),
                        {0, .5F, 1}));
  PS_CHECK(equal<float>(run("numeric.clamp", {array<float>({1})},
                            {{"min", -1e100}, {"max", 1e100}}),
                        {1}));
  PS_CHECK(run("numeric.clamp", {array<float>({1})},
               {{"min", 1e100}, {"max", 1e100}})
               .status()
               .code == ErrorCode::OperationFailed);
  for (const auto& samples :
       {array<float>({1, 2, 3}), array<double>({1, 2, 3})}) {
    PS_CHECK(equal<double>(run("numeric.mean", {samples}), {2}));
    auto variance = run("numeric.variance", {samples});
    PS_CHECK(variance.ok() &&
             std::abs(output(variance).as_float64().value() - 2. / 3) < 1e-15);
  }
  auto data = array<double>({1, 2, 3});
  auto reversed = Value::create(data.descriptor(), data.region(),
                                {0, {-8}, {2}}, data.copy_bytes())
                      .take_value();
  auto broadcast = Value::create(data.descriptor(), data.region(), {0, {0}},
                                 Value::from_float64(1).copy_bytes())
                       .take_value();
  PS_CHECK(equal<double>(run("numeric.add", {reversed, broadcast}), {4, 3, 2}));
  PS_CHECK(equal<double>(run("numeric.mean", {broadcast}), {1}));
  std::vector<std::uint8_t> unaligned(13);
  const float values[] = {.5F, 1.5F, 2.5F};
  std::memcpy(unaligned.data() + 1, values, 12);
  auto padded = Value::create({ElementType::Float32, {3}}, Region::whole({3}),
                              {1, {4}}, unaligned)
                    .take_value();
  PS_CHECK(equal<float>(run("numeric.add", {padded, array<float>({0, 0, 0})}),
                        {.5F, 1.5F, 2.5F}));
  auto half = array<float>({.75F});
  auto mask =
      Value::create({ElementType::Float32, {1, 1}}, Region::whole({1, 1}),
                    {0, {4, 4}}, half.copy_bytes(),
                    {encode_semantic(coverage_semantics()).take_value()})
          .take_value();
  auto twice = Value::create(mask.descriptor(), mask.region(), mask.layout(),
                             array<float>({2}).copy_bytes())
                   .take_value();
  auto old_mask = run("numeric.multiply", {mask, twice});
  PS_CHECK(old_mask.status().code == ErrorCode::TypeMismatch);
  return 0;
}
int failures_and_resources() {
  unsigned callbacks = 0;
  for (const auto& pair : std::vector<std::vector<Value>>{
           {array<std::int64_t>({1}), array<std::int64_t>({2})},
           {array<float>({1}), array<double>({2})},
           {array<float>({1}), array<float>({1, 2})}}) {
    PS_CHECK(run("numeric.add", pair, {}, &callbacks).status().code ==
             ErrorCode::TypeMismatch);
    PS_CHECK(callbacks == 0);
  }
  PS_CHECK(run("numeric.mean", {array<std::uint8_t>({1})}, {}, &callbacks)
                   .status()
                   .code == ErrorCode::TypeMismatch &&
           callbacks == 0);
  PS_CHECK(run("numeric.clamp", {array<float>({1})}, {{"min", 2.}, {"max", 1.}})
               .status()
               .code == ErrorCode::InvalidArgument);
  PS_CHECK(run("numeric.divide", {array<float>({1}), array<float>({-0.F})})
               .status()
               .code == ErrorCode::OperationFailed);
  PS_CHECK(run("numeric.multiply",
               {array<float>({std::numeric_limits<float>::max()}),
                array<float>({2})})
               .status()
               .code == ErrorCode::OperationFailed);
  PS_CHECK(run("numeric.variance",
               {array<double>({std::numeric_limits<double>::quiet_NaN()})})
               .status()
               .code == ErrorCode::OperationFailed);
  const auto n = UINT64_C(1) << 62;
  auto enormous = Value::create({ElementType::Float64, {n}}, Region::whole({n}),
                                {0, {0}}, Value::from_float64(1).copy_bytes())
                      .take_value();
  PS_CHECK(
      run("numeric.add", {enormous, enormous}, {}, &callbacks).status().code ==
          ErrorCode::ResourceExhausted &&
      callbacks == 0);
  ExecutionContextConfig limited;
  limited.maximum_live_bytes = 1;
  PS_CHECK(run("numeric.add", {array<float>({1}), array<float>({2})}, {},
               nullptr, limited)
               .status()
               .code == ErrorCode::ResourceExhausted);
  auto registry = make_default_operation_registry();
  CancellationSource cancelled;
  std::uint64_t live = 0;
  BufferAllocator allocator([&](std::uint64_t bytes) {
    live += bytes;
    cancelled.cancel();
    return Result<std::shared_ptr<void>>(
        std::shared_ptr<void>(new int(0), [&, bytes](void* pointer) {
          delete static_cast<int*>(pointer);
          live -= bytes;
        }));
  });
  const std::vector<Value> inputs{array<float>({1}), array<float>({2})};
  const std::vector<Region> demands{Region::whole({1}), Region::whole({1})};
  const Parameters parameters;
  auto result = registry->invoke(
      "numeric.add", {inputs, demands, parameters, Backend::Cpu,
                      cancelled.token(), Region::whole({1}), allocator});
  PS_CHECK(result.status().code == ErrorCode::Cancelled && live == 0);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(math_and_views() == 0);
  PS_CHECK(failures_and_resources() == 0);
  return 0;
}
