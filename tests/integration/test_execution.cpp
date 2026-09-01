#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/execution/execution.hpp"
#include "support/test_support.hpp"

namespace {

/**
 * @brief Compiles one document with explicit local GPU planning choice.
 * @param compiler Frozen-registry compiler.
 * @param graph Source graph context.
 * @param allow_gpu Whether optional GPU plans are permitted.
 * @return Complete compiled workflow.
 * @throws std::runtime_error If compilation unexpectedly fails.
 */
ps::CompiledWorkflow compile_or_throw(ps::Compiler* compiler,
                                      const ps::GraphContext& graph,
                                      bool allow_gpu = false) {
  auto compiled = compiler->compile(graph, ps::PlanningOptions{allow_gpu});
  if (!compiled.ok()) {
    throw std::runtime_error(compiled.status().message);
  }
  return compiled.take_value();
}

/**
 * @brief Builds a zero-input one-output source document.
 * @param operation Exact registered operation key.
 * @return Document selecting the operation output as `value`.
 * @throws std::bad_alloc If source storage allocation fails.
 */
ps::WorkflowDocument single_operation_document(const std::string& operation) {
  ps::WorkflowDocument document;
  document.nodes = {ps::WorkflowNode{1U, operation, {}, {}}};
  document.outputs = {ps::WorkflowOutput{"value", 1U, "value"}};
  return document;
}

}  // namespace

/**
 * @brief Exercises local execution, concurrency, cancellation, stale rejection,
 * GPU fallback, transfers, accounting cleanup, and raw benchmark observations.
 * @return Zero when all checks pass.
 * @throws std::bad_alloc If test setup allocation fails.
 * @throws std::runtime_error If an expected compilation cannot complete.
 * @note Behavioral failures otherwise return nonzero through `PS_CHECK`.
 */
int main() {
  using namespace ps;
  using namespace std::chrono_literals;

  auto operations = make_default_operation_registry();
  Compiler compiler(operations);
  ExecutionContext execution(operations,
                             ExecutionContextConfig{4U, false, 32U, 1024U});

  GraphContext graph(ps::test::addition_document(2.5, 4.0));
  CompiledWorkflow workflow = compile_or_throw(&compiler, graph);
  auto result = execution.execute(workflow.plan);
  PS_CHECK(result.ok());
  PS_CHECK(ps::test::named_scalar(result.value(), "sum") == 6.5);
  PS_CHECK(result.value().diagnostics.plan_digest ==
           workflow.plan.digest().value);
  PS_CHECK(!result.value().diagnostics.result_digest.empty());
  PS_CHECK(result.value().diagnostics.selected_backends.size() == 3U);

  auto other_operations = make_default_operation_registry();
  ExecutionContext mismatched_execution(
      other_operations, ExecutionContextConfig{1U, false, 8U, 1024U});
  auto mismatched_result = mismatched_execution.execute(workflow.plan);
  PS_CHECK(!mismatched_result.ok());
  PS_CHECK(mismatched_result.status().code == ErrorCode::Stale);

  CompiledWorkflow unavailable_gpu_workflow =
      compile_or_throw(&compiler, graph, true);
  auto unavailable_gpu_result =
      execution.execute(unavailable_gpu_workflow.plan);
  PS_CHECK(unavailable_gpu_result.ok());
  PS_CHECK(ps::test::named_scalar(unavailable_gpu_result.value(), "sum") ==
           6.5);
  PS_CHECK(unavailable_gpu_result.value().diagnostics.fallback_reasons.size() ==
           3U);
  PS_CHECK(unavailable_gpu_result.value().diagnostics.selected_backends.at(
               3U) == Backend::Cpu);

  GraphContext left(ps::test::addition_document(1.0, 2.0));
  GraphContext right(ps::test::addition_document(10.0, 20.0));
  CompiledWorkflow left_workflow = compile_or_throw(&compiler, left);
  CompiledWorkflow right_workflow = compile_or_throw(&compiler, right);
  auto left_future = std::async(std::launch::async, [&] {
    return execution.execute(left_workflow.plan);
  });
  auto right_future = std::async(std::launch::async, [&] {
    return execution.execute(right_workflow.plan);
  });
  auto left_result = left_future.get();
  auto right_result = right_future.get();
  PS_CHECK(left_result.ok());
  PS_CHECK(right_result.ok());
  PS_CHECK(ps::test::named_scalar(left_result.value(), "sum") == 3.0);
  PS_CHECK(ps::test::named_scalar(right_result.value(), "sum") == 30.0);

  GraphContext cancellable(ps::test::delayed_document(250));
  CompiledWorkflow cancellable_workflow =
      compile_or_throw(&compiler, cancellable);
  CancellationSource cancellation;
  auto cancelled_future = std::async(std::launch::async, [&] {
    return execution.execute(cancellable_workflow.plan, cancellation.token());
  });
  std::this_thread::sleep_for(20ms);
  PS_CHECK(cancellation.cancel());
  auto cancelled_result = cancelled_future.get();
  PS_CHECK(!cancelled_result.ok());
  PS_CHECK(cancelled_result.status().code == ErrorCode::Cancelled);

  GraphContext replaceable(ps::test::delayed_document(120));
  CompiledWorkflow stale_workflow = compile_or_throw(&compiler, replaceable);
  auto stale_future = std::async(std::launch::async, [&] {
    return execution.execute(stale_workflow.plan);
  });
  std::this_thread::sleep_for(20ms);
  replaceable.replace(ps::test::delayed_document(0));
  auto stale_result = stale_future.get();
  PS_CHECK(!stale_result.ok());
  PS_CHECK(stale_result.status().code == ErrorCode::Stale);

  WorkflowDocument fallback_document;
  fallback_document.nodes = {
      WorkflowNode{1U, "core.constant", {}, {{"value", 9.0}}},
      WorkflowNode{2U,
                   "core.gpu_fallback_probe",
                   {WorkflowInput{1U, "value"}},
                   {}},
  };
  fallback_document.outputs = {WorkflowOutput{"value", 2U, "value"}};
  GraphContext fallback_graph(std::move(fallback_document));
  CompiledWorkflow fallback_workflow =
      compile_or_throw(&compiler, fallback_graph, true);
  ExecutionContext gpu_execution(operations,
                                 ExecutionContextConfig{2U, true, 16U, 1024U});
  auto fallback_result = gpu_execution.execute(fallback_workflow.plan);
  PS_CHECK(fallback_result.ok());
  PS_CHECK(ps::test::named_scalar(fallback_result.value(), "value") == 9.0);
  PS_CHECK(fallback_result.value().diagnostics.fallback_reasons.size() == 1U);
  PS_CHECK(fallback_result.value().diagnostics.operation_timings.size() == 3U);
  PS_CHECK(fallback_result.value().diagnostics.transfer_count == 1U);
  PS_CHECK(fallback_result.value().diagnostics.transfer_bytes ==
           sizeof(double));
  PS_CHECK(fallback_result.value().diagnostics.selected_backends.at(2U) ==
           Backend::Cpu);

  auto facet_operations = std::make_shared<OperationRegistry>();
  OperationTraits facet_source_traits;
  facet_source_traits.output_element_type = ElementType::UInt8;
  PS_CHECK(facet_operations
               ->register_operation(OperationDefinition{
                   "test.facet_source", facet_source_traits,
                   [](const OperationInvocation&) -> Result<Value> {
                     return Value::create(
                         ValueDescriptor{ElementType::UInt8, {1U}},
                         Region::whole({1U}), StridedLayout{0U, {1}}, {7U},
                         {ValueFacet{"test.semantic", 3U, {4U, 5U}}});
                   }})
               .ok());
  OperationTraits facet_identity_traits;
  facet_identity_traits.input_count = 1U;
  facet_identity_traits.supports_gpu = true;
  facet_identity_traits.output_element_type = ElementType::UInt8;
  facet_identity_traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  facet_identity_traits.region_rule = OperationRegionRule::Elementwise;
  PS_CHECK(facet_operations
               ->register_operation(OperationDefinition{
                   "test.facet_identity", facet_identity_traits,
                   [](const OperationInvocation& invocation) -> Result<Value> {
                     return Result<Value>(invocation.inputs.front());
                   }})
               .ok());
  facet_operations->freeze();
  Compiler facet_compiler(facet_operations);
  ExecutionContext facet_execution(facet_operations,
                                   ExecutionContextConfig{1U, true, 8U, 1024U});
  WorkflowDocument facet_document;
  facet_document.nodes = {
      WorkflowNode{1U, "test.facet_source", {}, {}},
      WorkflowNode{2U, "test.facet_identity", {WorkflowInput{1U, "value"}}, {}},
  };
  facet_document.outputs = {WorkflowOutput{"faceted", 2U, "value"}};
  GraphContext facet_graph(std::move(facet_document));
  CompiledWorkflow facet_workflow =
      compile_or_throw(&facet_compiler, facet_graph, true);
  auto facet_result = facet_execution.execute(facet_workflow.plan);
  PS_CHECK(facet_result.ok());
  PS_CHECK(facet_result.value().diagnostics.transfer_count == 1U);
  const Value& faceted = facet_result.value().values.at("faceted");
  PS_CHECK(faceted.facets().size() == 1U);
  PS_CHECK(faceted.facets().front().key == "test.semantic");
  PS_CHECK(faceted.facets().front().version == 3U);
  PS_CHECK(faceted.facets().front().payload ==
           std::vector<std::uint8_t>({4U, 5U}));

  auto resource_operations = std::make_shared<OperationRegistry>();
  PS_CHECK(resource_operations
               ->register_operation(OperationDefinition{
                   "test.large",
                   OperationTraits{0U, true, true, true, false, false, 16U},
                   [](const OperationInvocation&) -> Result<Value> {
                     return Result<Value>(Value::from_float64(1.0));
                   }})
               .ok());
  PS_CHECK(resource_operations
               ->register_operation(OperationDefinition{
                   "test.small",
                   OperationTraits{0U, true, true, true, false, false, 8U},
                   [](const OperationInvocation&) -> Result<Value> {
                     return Result<Value>(Value::from_float64(2.0));
                   }})
               .ok());
  resource_operations->freeze();
  Compiler resource_compiler(resource_operations);
  ExecutionContext constrained(resource_operations,
                               ExecutionContextConfig{1U, false, 4U, 8U});
  GraphContext large(single_operation_document("test.large"));
  CompiledWorkflow large_workflow = compile_or_throw(&resource_compiler, large);
  auto large_result = constrained.execute(large_workflow.plan);
  PS_CHECK(!large_result.ok());
  PS_CHECK(large_result.status().code == ErrorCode::ResourceExhausted);
  GraphContext small(single_operation_document("test.small"));
  CompiledWorkflow small_workflow = compile_or_throw(&resource_compiler, small);
  auto small_result = constrained.execute(small_workflow.plan);
  PS_CHECK(small_result.ok());
  PS_CHECK(ps::test::named_scalar(small_result.value(), "value") == 2.0);
  PS_CHECK(small_result.value().diagnostics.peak_live_bytes == 8U);

  RawBenchmarkRunner benchmark(&compiler, &execution);
  RawBenchmarkOptions benchmark_options;
  benchmark_options.iterations = 2U;
  benchmark_options.correctness_oracle = [](const ExecutionResult& observed) {
    const bool accepted = ps::test::named_scalar(observed, "sum") == 6.5;
    return CorrectnessObservation{accepted,
                                  accepted ? std::string() : "wrong sum"};
  };
  auto report = benchmark.run(graph, benchmark_options);
  PS_CHECK(report.ok());
  PS_CHECK(report.value().samples.size() == 2U);
  for (const RawBenchmarkSample& sample : report.value().samples) {
    PS_CHECK(sample.outcome == ErrorCode::Ok);
    PS_CHECK(sample.correctness_checked);
    PS_CHECK(sample.correctness_accepted);
    PS_CHECK(!sample.execution.plan_digest.empty());
  }
  return 0;
}
