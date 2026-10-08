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

#include "../../examples/numeric_workflow/result_fixture.hpp"
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
struct SpyPhase {
  ResultContinuation inner;
  explicit SpyPhase(ResultContinuation program) : inner(std::move(program)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    return inner.poll(phase);
  }
};
struct PreparedSpy {
  std::shared_ptr<const PreparedOperation> inner;
};
Result<ExecutionResult> run(const std::string& operation,
                            const std::vector<Value>& inputs,
                            const Parameters& parameters = {},
                            unsigned* calls = nullptr,
                            ExecutionContextConfig config = {},
                            CancellationToken cancellation = {}) {
  auto base = make_default_operation_registry();
  auto traits = base->find_traits(operation).take_value();
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition spy;
  spy.key = operation;
  spy.traits = traits;
  spy.traits.requires_metadata_specialization = true;
  spy.traits.outputs[0].continuation_bytes += sizeof(SpyPhase);
  spy.prepare_static = [base, operation](const auto& metadata,
                                         const auto& parameters) {
    auto inner = base->prepare_operation(operation, metadata, parameters);
    if (!inner.ok())
      return Result<OperationPreparation>(inner.status());
    OperationPreparation result;
    result.outputs.resize(1);
    result.outputs[0].metadata.result_schema = std::make_shared<SchemaTemplate>(
        *inner.value()->traits().outputs[0].result_schema);
    result.state =
        std::make_shared<PreparedSpy>(PreparedSpy{inner.take_value()});
    return Result<OperationPreparation>(std::move(result));
  };
  spy.start_result = [base, operation, calls](const auto& query,
                                              const auto& allocator) {
    if (calls)
      ++*calls;
    auto forwarded = query;
    forwarded.prepared =
        static_cast<const PreparedSpy*>(query.prepared->state())->inner;
    auto inner = base->start_result(operation, forwarded, allocator);
    if (!inner.ok())
      return inner;
    return ResultContinuation::make<SpyPhase>(allocator, inner.take_value());
  };
  auto status = registry->register_operation(std::move(spy));
  if (!status.ok())
    return Result<ExecutionResult>(status);
  status = registry->freeze();
  if (!status.ok())
    return Result<ExecutionResult>(status);
  WorkflowDocument document;
  std::vector<WorkflowInput> references;
  for (size_t i = 0; i < inputs.size(); ++i) {
    WorkflowInputDeclaration input;
    input.id = i + 1;
    input.name = "input" + std::to_string(i);
    input.result_schema = std::make_shared<SchemaTemplate>(
        numeric_result_fixture::source_schema(inputs[i]));
    document.inputs.push_back(std::move(input));
    references.push_back(WorkflowInputReference{i + 1});
  }
  document.nodes = {{100, operation, references, parameters}};
  document.outputs = {{"result", 100, "value"}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  if (!compiled.ok())
    return Result<ExecutionResult>(compiled.status());
  if (!config.managed_resources) {
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Payload] =
        config.maximum_live_bytes;
  }
  ExecutionContext context(registry, config);
  auto budget = context.resource_budget();
  if (!budget.ok())
    return Result<ExecutionResult>(budget.status());
  ExecutionBindings bindings;
  // Input assembly imports only physical source storage, preserving signed and
  // zero strides; the operation itself reads the authorized Result windows.
  for (size_t i = 0; i < inputs.size(); ++i) {
    const auto& value = inputs[i];
    auto buffer = budget.value().allocator().allocate(value.bytes().size());
    if (!buffer.ok())
      return Result<ExecutionResult>(buffer.status());
    auto storage = buffer.take_value();
    std::memcpy(storage.data(), value.bytes().data(), value.bytes().size());
    auto builder = ResultBuilder::start(
        budget.value(), *document.inputs[i].result_schema, "finite.source");
    if (!builder.ok())
      return Result<ExecutionResult>(builder.status());
    auto publisher = builder.take_value();
    auto relation = ResultRelation::cartesian(budget.value(), 1, {0, 8, 0, 0});
    if (!relation.ok())
      return Result<ExecutionResult>(relation.status());
    status = publisher.bind_descriptor_relation(relation.take_value());
    if (!status.ok())
      return Result<ExecutionResult>(status);
    relation = ResultRelation::cartesian(
        budget.value(), value.region().element_count().value(), {0, 1, 0, 0});
    if (!relation.ok())
      return Result<ExecutionResult>(relation.status());
    status = publisher.publish_tensor(
        0, value.region(), value.layout(), std::move(storage).freeze(),
        relation.take_value(), {true, true, true, true});
    if (!status.ok())
      return Result<ExecutionResult>(status);
    auto result = publisher.seal();
    if (!result.ok())
      return Result<ExecutionResult>(result.status());
    bindings.inputs.push_back({document.inputs[i].name, result.take_value()});
  }
  return context.execute(compiled.value().plan, bindings, cancellation);
}
template <class T>
bool equal(const Result<ExecutionResult>& result,
           const std::vector<T>& expected) {
  if (!result.ok()) {
    std::cerr << result.status().message
              << " code=" << static_cast<unsigned>(result.status().code)
              << " reason=" << static_cast<unsigned>(result.status().reason)
              << "\n";
    return false;
  }
  const auto& value = result.value().results.at("result");
  const auto& tensor = value.schema().tensors[0];
  if (tensor.descriptor.element_type != type<T>() || !tensor.facets.empty() ||
      tensor.sample_count().value() != expected.size())
    return false;
  auto descriptor = value.descriptor();
  if (!descriptor.ok())
    return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    T actual;
    if (!value.read_tensor(descriptor.value(), 0, {i}, &actual, sizeof(T))
             .ok() ||
        std::memcmp(&actual, &expected[i], sizeof(T)))
      return false;
  }
  return true;
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
    double actual = 0;
    PS_CHECK(variance.ok());
    const auto& result = variance.value().results.at("result");
    PS_CHECK(
        result.read_tensor(result.descriptor().take_value(), 0, {0}, &actual, 8)
            .ok() &&
        std::abs(actual - 2. / 3) < 1e-15);
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
  CancellationSource cancelled;
  cancelled.cancel();
  auto result = run("numeric.add", {array<float>({1}), array<float>({2})}, {},
                    nullptr, {}, cancelled.token());
  PS_CHECK(!result.ok() && result.status().code == ErrorCode::Cancelled);
  auto registry = make_default_operation_registry();
  CancellationSource allocating_cancel;
  std::uint64_t live = 0, allocations = 0;
  BufferAllocator allocator([&](std::uint64_t bytes) {
    ++allocations;
    live += bytes;
    allocating_cancel.cancel();
    return Result<std::shared_ptr<void>>(
        std::shared_ptr<void>(new int(0), [&, bytes](void* pointer) {
          delete static_cast<int*>(pointer);
          live -= bytes;
        }));
  });
  {
    ResourceBudget root;
    const std::vector<Value> inputs{array<float>({1}), array<float>({2})};
    std::vector<OperationMetadata> metadata_inputs(2);
    for (unsigned i = 0; i < 2; ++i)
      metadata_inputs[i].result_schema = std::make_shared<const SchemaTemplate>(
          numeric_result_fixture::source_schema(inputs[i]));
    const Parameters parameters;
    auto prepared =
        registry->prepare_operation("numeric.add", metadata_inputs, parameters)
            .take_value();
    auto metadata_outputs =
        infer_operation_outputs(prepared->traits(), metadata_inputs, parameters)
            .take_value();
    ResultProgramMetadata metadata{metadata_inputs, metadata_outputs[0]};
    ResultProgramQuery query(metadata, parameters);
    query.prepared = prepared;
    query.semantic_key = "cancel-during-state-allocation";
    query.cancellation = allocating_cancel.token();
    auto started = registry->start_result("numeric.add", query, allocator);
    PS_CHECK(allocations > 0 && allocating_cancel.token().cancelled());
    if (started.ok()) {
      auto continuation = started.take_value();
      ResultObjectInputs objects;
      ResourceVector<ResultIoReply> io;
      ResultProgramPhase phase{
          query, objects,
          io,    allocator,
          root,  [&](std::uint64_t amount) { return root.consume({amount}); },
          {}};
      auto stopped = continuation.poll(phase);
      PS_CHECK(!stopped.ok() && stopped.status().code == ErrorCode::Cancelled);
    } else {
      PS_CHECK(started.status().code == ErrorCode::Cancelled);
    }
    for (auto used : root.statistics().live.values)
      PS_CHECK(used == 0);
  }
  PS_CHECK(live == 0);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(math_and_views() == 0);
  PS_CHECK(failures_and_resources() == 0);
  return 0;
}
