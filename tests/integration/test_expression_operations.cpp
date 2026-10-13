#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Parameters = std::map<std::string, ParameterValue>;
template <class T>
Value array(const std::vector<T>& samples,
            std::vector<std::uint64_t> shape = {},
            const std::vector<ValueFacet>& facets = {}) {
  if (shape.empty())
    shape = {samples.size()};
  const auto type =
      std::is_same_v<T, double> ? ElementType::Float64 : ElementType::Float32;
  auto made = MutableValue::allocate({type, shape}, Region::whole(shape),
                                     BufferAllocator{});
  auto value = made.take_value();
  std::memcpy(value.data(), samples.data(), samples.size() * sizeof(T));
  return std::move(value).publish(facets).take_value();
}
SemanticDescriptor signal_semantic(
    double start = 0, double step = 1,
    const std::string& unit = "dimensionless",
    const std::string& axis = "dimensionless",
    SemanticKind kind = SemanticKind::SampledSignal) {
  SemanticDescriptor s;
  s.kind = kind;
  s.unit = unit;
  s.channels = {{"value", "value", unit}};
  s.sample_origin = start;
  s.sample_step = step;
  s.sample_axis_unit = axis;
  return s;
}
Value signal(const std::vector<float>& samples,
             const SemanticDescriptor& s = signal_semantic()) {
  return array(samples, {}, {encode_semantic(s).take_value()});
}
Parameters parameters(const std::string& expression, std::int64_t count = 3,
                      double start = 0, double step = .5) {
  return {{"expression", expression},
          {"count", count},
          {"start", start},
          {"step", step}};
}
WorkflowDocument document(const std::vector<Value>& inputs,
                          const std::vector<WorkflowNode>& nodes) {
  WorkflowDocument d;
  numeric_result_fixture::declare_sources(&d, inputs);
  d.nodes = nodes;
  d.outputs = {{"result", nodes.back().id, "value"}};
  return d;
}
Result<ExecutionResult> run(const std::vector<Value>& inputs,
                            const std::vector<WorkflowNode>& nodes,
                            std::shared_ptr<OperationRegistry> registry = {}) {
  if (!registry)
    registry = make_default_operation_registry();
  GraphContext graph(document(inputs, nodes));
  auto compiled = Compiler(registry).compile(graph);
  if (!compiled.ok())
    return Result<ExecutionResult>(compiled.status());
  ExecutionContext execution(registry);
  try {
    auto bound = numeric_result_fixture::bind_sources(
        execution.resource_budget().take_value(), inputs);
    return execution.execute(compiled.value().plan, bound);
  } catch (const std::exception& error) {
    return Result<ExecutionResult>(
        Status{ErrorCode::InvalidArgument, error.what()});
  }
}
Value output(const Result<ExecutionResult>& result) {
  const auto& object = result.value().results.at("result");
  const auto& spec = object.schema().tensors[0];
  const auto shape = spec.sample_shape();
  auto value = MutableValue::allocate({spec.descriptor.element_type, shape},
                                      Region::whole(shape), BufferAllocator{})
                   .take_value();
  auto count = Region::whole(shape).element_count().take_value();
  const auto width = Value::element_size(spec.descriptor.element_type);
  std::vector<std::uint64_t> at(shape.size());
  auto facts = object.descriptor().take_value();
  for (std::uint64_t i = 0; i < count; ++i) {
    auto status =
        object.read_tensor(facts, 0, at, value.data() + i * width, width);
    if (!status.ok())
      throw std::runtime_error(status.message);
    for (std::size_t axis = shape.size(); axis-- > 0;)
      if (++at[axis] < shape[axis])
        break;
      else
        at[axis] = 0;
  }
  return std::move(value).publish(spec.facets).take_value();
}
bool equal(const Result<ExecutionResult>& result,
           const std::vector<float>& expected) {
  if (!result.ok()) {
    std::cerr << "unexpected expression failure: " << result.status().message
              << '\n';
    return false;
  }
  return output(result).bytes().size() == expected.size() * 4 &&
         std::memcmp(output(result).bytes().data(), expected.data(),
                     expected.size() * 4) == 0;
}
Result<ExecutionResult> sample(const std::string& source,
                               const std::vector<double>& coefficients = {1},
                               std::int64_t count = 3, double start = 0,
                               double step = .5) {
  return run({array(coefficients)}, {{1,
                                      "numeric.sample_expression",
                                      {WorkflowInputReference{1}},
                                      parameters(source, count, start, step)}});
}
std::string balanced(unsigned leaves) {
  if (leaves == 1)
    return "1";
  return "(" + balanced(leaves / 2) + "+" + balanced(leaves - leaves / 2) + ")";
}
int expressions() {
  PS_CHECK(equal(sample("x^2"), {0, .25F, 1}));
  auto five = sample("c[0]*x+c[1]", {2, 1}, 5, 0, .25);
  PS_CHECK(equal(five, {1, 1.5F, 2, 2.5F, 3}));
  auto metadata = decode_semantic(output(five).facets()[0]);
  PS_CHECK(metadata.ok() &&
           metadata.value().kind == SemanticKind::SampledSignal &&
           metadata.value().channels[0].role == "value" &&
           metadata.value().unit == "dimensionless" &&
           metadata.value().sample_step == .25 &&
           metadata.value().sample_axis_unit == "dimensionless");
  for (const auto& test : std::vector<std::pair<std::string, float>>{
           {"-2^2", -4},
           {"2^-2", .25F},
           {"2^3^2", 512},
           {"(-2)^2", 4},
           {"0^0", 1},
           {"1.25e2 + .5 - 2.5E+1", 100.5F},
           {"abs(-2)+sqrt(4)+exp(0)+log(1)+sin(0)+cos(0)", 6},
           {"min(2,max(-3,1))", 1},
           {"--3", 3},
           {"2*-3", -6}})
    PS_CHECK(equal(sample(test.first, {1}, 1), {test.second}));
  PS_CHECK(equal(sample(std::string(31, '-') + "1", {1}, 1), {-1}));
  PS_CHECK(sample(std::string(32, '-') + "1", {1}, 1).status().code ==
           ErrorCode::InvalidArgument);
  std::string chain = "1";
  for (unsigned i = 1; i < 32; ++i)
    chain += "+1";
  PS_CHECK(equal(sample(chain, {1}, 1), {32}));
  PS_CHECK(sample(chain + "+1", {1}, 1).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(
      equal(sample("+" + balanced(128), {1}, 1), {128}));  // 256 actual nodes.
  PS_CHECK(sample("++" + balanced(128), {1}, 1).status().code ==
           ErrorCode::InvalidArgument);
  const auto nested = std::string(2047, '(') + "x" + std::string(2047, ')');
  PS_CHECK(equal(sample(nested + " ", {1}, 1, .25),
                 {.25F}));  // 4096 bytes, one AST node.
  PS_CHECK(sample(nested + "  ", {1}, 1).status().code ==
           ErrorCode::InvalidArgument);
  for (const auto* bad :
       {"nan", "inf", "0x1p0", "x;1", "sqrt()", "min(1)", "max(1,2,3)", "c[-1]",
        "c[1]", "1e", "1e9999", "1..2", "1+", "(", "[x]"})
    PS_CHECK(sample(bad, {1}, 1).status().code == ErrorCode::InvalidArgument);
  for (const auto* bad : {"1/0", "sqrt(-1)", "log(0)", "exp(1000)", "(-1)^.5",
                          "min(1e300*1e300,0)", "1e100"})
    PS_CHECK(sample(bad, {1}, 1).status().code == ErrorCode::OperationFailed);
  PS_CHECK(sample("1", {std::numeric_limits<double>::quiet_NaN()}, 1)
               .status()
               .code == ErrorCode::OperationFailed);
  PS_CHECK(sample("x", {1}, 0).status().code == ErrorCode::InvalidArgument);
  PS_CHECK(sample("x", {1}, 1048577).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(sample("x", std::vector<double>(257, 1), 1).status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(equal(sample("c[255]", std::vector<double>(256, 2), 1), {2}));
  for (const double step : {0., -1., std::numeric_limits<double>::infinity()})
    PS_CHECK(sample("1", {1}, 3, 0, step).status().code ==
             ErrorCode::InvalidArgument);
  PS_CHECK(sample("1", {1}, 2, std::numeric_limits<double>::max(),
                  std::numeric_limits<double>::max())
               .status()
               .code == ErrorCode::InvalidArgument);
  PS_CHECK(sample("1", {1}, 2, 1, std::numeric_limits<double>::denorm_min())
               .status()
               .code == ErrorCode::InvalidArgument);
  auto registry = make_default_operation_registry();
  GraphContext largest(
      document({array<double>({1})}, {{1,
                                       "numeric.sample_expression",
                                       {WorkflowInputReference{1}},
                                       parameters("1", 1048576, 0, 1)}}));
  auto plan = Compiler(registry).compile(largest);
  PS_CHECK(plan.ok() && plan.value()
                                .plan.steps()[0]
                                .output_result_schema->tensors[0]
                                .sample_shape()[0] == 1048576);
  return 0;
}
int luts() {
  const auto coefficient = array<double>({1});
  const auto query =
      signal({.25F}, signal_semantic(10, 2, "dimensionless", "seconds"));
  auto generated =
      run({coefficient, query},
          {{1,
            "numeric.sample_expression",
            {WorkflowInputReference{1}},
            parameters("x^2")},
           {2,
            "lut.apply_1d",
            {WorkflowInputReference{2}, WorkflowNodeOutput{1, "value"}},
            {{"out_of_domain", std::string("reject")}}}});
  PS_CHECK(equal(generated, {.125F}) && output(generated).facets().empty());
  auto table = signal({0, .25F, 1}, signal_semantic(0, .5));
  auto apply = [&](const Value& q, const Value& t,
                   const std::string& policy = "reject") {
    return run({q, t}, {{1,
                         "lut.apply_1d",
                         {WorkflowInputReference{1}, WorkflowInputReference{2}},
                         {{"out_of_domain", policy}}}});
  };
  PS_CHECK(apply(signal({-1, 2}), table).status().code ==
           ErrorCode::OperationFailed);
  PS_CHECK(equal(apply(signal({-1, 0, .25F, 1, 2}), table, "clip"),
                 {0, 0, .125F, 1, 1}));
  const auto limit = std::numeric_limits<float>::max();
  PS_CHECK(equal(apply(signal({.5F}), signal({-limit, limit})), {0}));
  // Independent binary-rational/Fraction oracles near a sampling endpoint.
  const auto near_endpoint = signal_semantic(-1, 0x1.0000000000001p+0);
  PS_CHECK(equal(apply(signal({0}), signal({1e30F, 0}, near_endpoint)),
                 {222044608266240.F}));
  PS_CHECK(equal(
      apply(signal({0}), signal({0x1p100F, -0x1p48F}, near_endpoint)), {0}));
  PS_CHECK(equal(
      apply(signal({1}),
            signal({1e30F, 2e30F},
                   signal_semantic(0, std::numeric_limits<double>::max()))),
      {1e30F}));
  PS_CHECK(
      equal(apply(signal({.25F}), signal({1, 0, -1}, signal_semantic(0, .5))),
            {.5F}));
  auto physical_table = signal(
      {0, 2}, signal_semantic(0, 1, "nits", "seconds", SemanticKind::Lut));
  PS_CHECK(
      equal(apply(signal({.25F}, signal_semantic(10, .5, "seconds", "pixels")),
                  physical_table),
            {.5F}));
  PS_CHECK(apply(query, physical_table).status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(apply(query, signal({0})).status().code == ErrorCode::TypeMismatch);
  PS_CHECK(apply(query, array<float>({0, 1})).status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(apply(query, table, "wrap").status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(apply(signal({std::numeric_limits<float>::quiet_NaN()}), table)
               .status()
               .code == ErrorCode::InvalidArgument);
  PS_CHECK(
      apply(query,
            signal({0, 1}, signal_semantic(
                               1, std::numeric_limits<double>::denorm_min())))
          .status()
          .code == ErrorCode::TypeMismatch);
  auto multichannel = signal_semantic();
  multichannel.channels.push_back({"other", "value", "dimensionless"});
  auto multi = array<float>({0, .25F, .75F, 1}, {2, 2},
                            {encode_semantic(multichannel).take_value()});
  auto result = apply(multi, table);
  PS_CHECK(equal(result, {0, .125F, .625F, 1}) &&
           output(result).descriptor().shape ==
               (std::vector<std::uint64_t>{2, 2}));
  return 0;
}
int allocation_cancellation() {
  auto registry = make_default_operation_registry();
  ExecutionContext execution(registry);
  auto root = execution.resource_budget().take_value();
  const std::vector<Value> input{array<double>({1})};
  const std::vector<WorkflowNode> nodes{{1,
                                         "numeric.sample_expression",
                                         {WorkflowInputReference{1}},
                                         parameters("x^2")}};
  auto doc = document(input, nodes);
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto bound = numeric_result_fixture::bind_sources(root, input);
  const auto baseline = root.statistics().live[ResourceKind::Payload];
  CancellationSource stop;
  stop.cancel();
  auto cancelled = execution.execute(plan, bound, stop.token());
  PS_CHECK(!cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled &&
           root.statistics().live[ResourceKind::Payload] == baseline);
  ExecutionOptions options;
  options.maximum_dependency_work = 1;
  auto frozen = execution.freeze(plan, bound).take_value();
  auto exhausted = execution.execute(frozen, {}, options);
  PS_CHECK(!exhausted.ok() &&
           exhausted.status().code == ErrorCode::ResourceExhausted &&
           root.statistics().live[ResourceKind::Payload] == baseline);
  return 0;
}
int result_contracts() {
  auto registry = make_default_operation_registry();
  ExecutionContext execution(registry);
  auto root = execution.resource_budget().take_value();
  const std::vector<Value> input{array<double>({1})};
  const std::vector<WorkflowNode> nodes{{1,
                                         "numeric.sample_expression",
                                         {WorkflowInputReference{1}},
                                         parameters("c[0]*x")}};
  auto doc = document(input, nodes);
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto bound = numeric_result_fixture::bind_sources(root, input);
  auto first = execution.execute(plan, bound);
  PS_CHECK(equal(first, {0, .5F, 1}));
  bound.inputs[0].result =
      numeric_result_fixture::source(root, array<double>({2}));
  PS_CHECK(equal(execution.execute(plan, bound), {0, 1, 2}));
  auto dirty = first.value().dependencies.potential_dirty(
      "input0", Footprint::all({1}).take_value(), 4, {},
      ResultSupportTarget::Tensor, 0);
  PS_CHECK(dirty.ok() &&
           dirty.value().at("result") == Footprint::all({3}).take_value());
  auto packed = array<double>({3, 2});
  std::vector<std::uint8_t> packed_bytes(
      packed.bytes().data(), packed.bytes().data() + packed.bytes().size());
  auto reversed = Value::create({ElementType::Float64, {2}}, Region::whole({2}),
                                {8, {-8}}, std::move(packed_bytes))
                      .take_value();
  PS_CHECK(equal(run({reversed}, {{1,
                                   "numeric.sample_expression",
                                   {WorkflowInputReference{1}},
                                   parameters("c[0]*x+c[1]")}}),
                 {3, 4, 5}));
  auto frozen = execution.freeze(plan, bound).take_value();
  auto empty = execution.execute_fragments(frozen, {{"result", {false, {}}}});
  PS_CHECK(empty.ok() &&
           empty.value()
               .results.at("result")
               .descriptor()
               .value()
               .tensor_coverage(0)
               .empty() &&
           empty.value().dependencies.source_support().value().empty());
  bound.inputs[0].result =
      numeric_result_fixture::source(root, array<double>({NAN}));
  frozen = execution.freeze(plan, bound).take_value();
  PS_CHECK(execution.execute_fragments(frozen, {{"result", {false, {}}}}).ok());
  const std::vector<Value> lookup{signal({.25F}), signal({0, 1, 2})};
  const std::vector<WorkflowNode> lookup_nodes{
      {1,
       "lut.apply_1d",
       {WorkflowInputReference{1}, WorkflowInputReference{2}},
       {{"out_of_domain", std::string("reject")}}}};
  GraphContext lookup_graph(document(lookup, lookup_nodes));
  auto lookup_plan = Compiler(registry).compile(lookup_graph).take_value().plan;
  auto lookup_bindings = numeric_result_fixture::bind_sources(root, lookup);
  auto lookup_frozen =
      execution.freeze(lookup_plan, lookup_bindings).take_value();
  auto lookup_empty =
      execution.execute_fragments(lookup_frozen, {{"result", {false, {}}}});
  PS_CHECK(lookup_empty.ok() &&
           lookup_empty.value().dependencies.source_support().value().empty());
  lookup_bindings.inputs[1].result =
      numeric_result_fixture::source(root, signal({0, 1, NAN}));
  lookup_frozen = execution.freeze(lookup_plan, lookup_bindings).take_value();
  PS_CHECK(execution.execute_fragments(lookup_frozen, {{"result", {false, {}}}})
               .ok());
  auto failed = execution.execute(lookup_frozen);
  PS_CHECK(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument);
  auto batched_storage = array<float>({0, .25F, .5F, 1}, {2, 2});
  auto batched_schema = numeric_result_fixture::source_schema(batched_storage);
  batched_schema.tensors[0].descriptor.shape = {2};
  batched_schema.tensors[0].batch_axes = {2};
  batched_schema.tensors[0].facets = {
      encode_semantic(signal_semantic()).take_value()};
  auto batched_doc = document({batched_storage, lookup[1]}, lookup_nodes);
  batched_doc.inputs[0].result_schema =
      std::make_shared<SchemaTemplate>(batched_schema);
  GraphContext batched_graph(batched_doc);
  auto batched_plan =
      Compiler(registry).compile(batched_graph).take_value().plan;
  auto batched_bindings = numeric_result_fixture::bind_sources(
      root, {batched_storage, lookup[1]}, &batched_doc);
  auto batched_result = execution.execute(batched_plan, batched_bindings);
  PS_CHECK(equal(batched_result, {0, .25F, .5F, 1}) &&
           batched_result.value()
                   .results.at("result")
                   .schema()
                   .tensors[0]
                   .batch_axes == ResourceVector<std::uint64_t>({2}));
  ExecutionContext constrained(registry, {1, false, 8, 64});
  auto constrained_root = constrained.resource_budget().take_value();
  auto constrained_bindings =
      numeric_result_fixture::bind_sources(constrained_root, input);
  const auto before = constrained_root.statistics().live[ResourceKind::Payload];
  auto allocation = constrained.execute(plan, constrained_bindings);
  PS_CHECK(!allocation.ok() &&
           allocation.status().code == ErrorCode::ResourceExhausted &&
           constrained_root.statistics().live[ResourceKind::Payload] == before);
  GraphContext large(
      document(input, {{1,
                        "numeric.sample_expression",
                        {WorkflowInputReference{1}},
                        parameters(balanced(64), 1048576, 0, 1)}}));
  auto large_plan = Compiler(registry).compile(large).take_value().plan;
  auto large_bindings = numeric_result_fixture::bind_sources(root, input);
  const auto baseline = root.statistics().live[ResourceKind::Payload];
  const auto work = root.statistics().issued.work;
  CancellationSource stop;
  ExecutionOptions options;
  options.maximum_dependency_work = UINT64_C(1) << 30;
  auto large_frozen = execution.freeze(large_plan, large_bindings).take_value();
  auto active = std::async(std::launch::async, [&] {
    return execution.execute(large_frozen, stop.token(), options);
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (root.statistics().issued.work < work + 50000 &&
         active.wait_for(std::chrono::milliseconds(0)) !=
             std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool progressed = root.statistics().issued.work >= work + 50000;
  stop.cancel();
  auto stopped = active.get();
  PS_CHECK(progressed && !stopped.ok() &&
           stopped.status().code == ErrorCode::Cancelled &&
           root.statistics().live[ResourceKind::Payload] == baseline);
  return 0;
}
int c_contract() {
#ifdef PS_EXPRESSION_CONTRACT_FIXTURE
  auto registry = std::make_shared<OperationRegistry>();
  const auto loaded = registry->load_plugin(PS_EXPRESSION_CONTRACT_FIXTURE);
  if (!loaded.ok())
    std::cerr << loaded.message << '\n';
  PS_CHECK(loaded.ok() && registry->freeze().ok());
  auto invoke = [&](const std::string& expression, const Value& coefficients,
                    std::int64_t count, double start = 0, double step = .5) {
    return run({coefficients},
               {{1,
                 "fixture.expression_contract",
                 {WorkflowInputReference{1}},
                 parameters(expression, count, start, step)}},
               registry);
  };
  auto result = invoke("x+c[0]", array<double>({1}), 3);
  PS_CHECK(equal(
      result, {0, 0, 0}));  // Fixture materializes host-inferred zero samples.
  PS_CHECK(decode_semantic(output(result).facets()[0]).value().sample_step ==
           .5);
  PS_CHECK(invoke("c[1]", array<double>({1}), 3).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(
      invoke("x", array<double>(std::vector<double>(257)), 3).status().code ==
      ErrorCode::TypeMismatch);
  PS_CHECK(invoke("x", array<double>({1}), 1048577).status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(equal(invoke("abs(-2)+sqrt(4)+exp(0)+log(1)+sin(x)+cos(0)+c[0]^2",
                        array<double>({2}), 65),
                 std::vector<float>(65, 0)));
  PS_CHECK(equal(invoke("1/0", array<double>({1}), 1), {0}));
  PS_CHECK(equal(
      invoke("c[255]", array<double>(std::vector<double>(256, 1)), 1), {0}));
  PS_CHECK(equal(invoke("+" + balanced(128), array<double>({1}), 1), {0}));
  PS_CHECK(invoke("++" + balanced(128), array<double>({1}), 1).status().code ==
           ErrorCode::InvalidArgument);
  const auto nested = std::string(2047, '(') + "x" + std::string(2047, ')');
  PS_CHECK(equal(invoke(nested + " ", array<double>({1}), 1), {0}));
  PS_CHECK(invoke(nested + "  ", array<double>({1}), 1).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(invoke("x", array<double>({1}, {}, {{"vendor.note", 1, {1}}}), 1)
               .status()
               .code == ErrorCode::TypeMismatch);
  PS_CHECK(invoke("x", array<double>({1}), 1, 0, 0).status().code ==
           ErrorCode::InvalidArgument);
  auto shifted = invoke("x", array<double>({1}), 3, -2, .25);
  PS_CHECK(shifted.ok());
  const auto& shifted_result = shifted.value().results.at("result");
  auto semantic = decode_semantic(shifted_result.schema().tensors[0].facets[0]);
  PS_CHECK(semantic.ok() && semantic.value().sample_origin == -2 &&
           semantic.value().sample_step == .25);
  auto original = decode_semantic(
      result.value().results.at("result").schema().tensors[0].facets[0]);
  PS_CHECK(original.ok() && original.value().sample_origin == 0 &&
           original.value().sample_step == .5);
  const auto coefficients = array<double>({1});
  auto empty_document =
      document({coefficients}, {{1,
                                 "fixture.expression_contract",
                                 {WorkflowInputReference{1}},
                                 parameters("x", 65)}});
  GraphContext empty_graph(empty_document);
  auto empty_plan = Compiler(registry).compile(empty_graph).take_value().plan;
  ExecutionContext context(registry);
  auto root = context.resource_budget().take_value();
  auto bound = numeric_result_fixture::bind_sources(root, {coefficients});
  auto frozen = context.freeze(empty_plan, bound).take_value();
  auto empty = context.execute_fragments(frozen, {{"result", {false, {}}}});
  PS_CHECK(empty.ok());
  PS_CHECK(empty.value()
               .results.at("result")
               .descriptor()
               .value()
               .tensor_coverage(0)
               .empty());
  CancellationSource stop;
  stop.cancel();
  auto cancelled = context.execute(frozen, stop.token());
  PS_CHECK(!cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled);
#endif
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(expressions() == 0);
  PS_CHECK(luts() == 0);
  PS_CHECK(allocation_cancellation() == 0);
  PS_CHECK(result_contracts() == 0);
  PS_CHECK(c_contract() == 0);
  return 0;
}
