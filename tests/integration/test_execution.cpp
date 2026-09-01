#include <chrono>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "benchmark/raw_benchmark_test_hooks.hpp"
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
  ps::PlanningOptions options;
  options.allow_gpu = allow_gpu;
  auto compiled = compiler->compile(graph, options);
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

/**
 * @brief Builds one dense rank-one Float64 Value of the requested extent.
 * @param extent Positive logical element count.
 * @return Whole-Region immutable zero-filled Value.
 * @throws std::bad_alloc If Value storage allocation fails.
 * @throws std::invalid_argument If extent is zero.
 */
ps::Value vector_value(std::uint64_t extent) {
  if (extent == 0U) {
    throw std::invalid_argument("vector fixture extent must be positive");
  }
  auto value = ps::Value::create(
      ps::ValueDescriptor{ps::ElementType::Float64, {extent}},
      ps::Region::whole({extent}), ps::StridedLayout{0U, {8}},
      std::vector<std::uint8_t>(
          static_cast<std::size_t>(extent) * sizeof(double), 0U));
  if (!value.ok()) {
    throw std::runtime_error(value.status().message);
  }
  return value.take_value();
}

/**
 * @brief Compares one rank-one Region with expected offset and extent.
 * @param region Candidate logical Region.
 * @param offset Expected zero-based offset.
 * @param extent Expected logical element count.
 * @return True only for the exact rank-one interval.
 * @throws Nothing.
 */
bool region_equals(const ps::Region& region, std::uint64_t offset,
                   std::uint64_t extent) noexcept {
  return region.rank() == 1U && region.dimensions().front().offset == offset &&
         region.dimensions().front().extent == extent;
}

/**
 * @brief Builds one frozen registry whose shared test operation uses a rule.
 * @param rule Whole, elementwise-exact, or halo input-demand behavior.
 * @param halo_radius Positive only for `Halo`.
 * @param observed Callback-owned location receiving the executed input demand.
 * @return Frozen registry with fixed-shape source and `test.region` consumer.
 * @throws std::bad_alloc If registry/callback storage allocation fails.
 * @throws std::logic_error If fixture registration unexpectedly fails.
 * @note Each returned registry is independent but uses identical operation
 * keys so plan differences originate in semantic traits and demands.
 */
std::shared_ptr<ps::OperationRegistry> make_region_registry(
    ps::OperationRegionRule rule, std::uint32_t halo_radius,
    std::shared_ptr<ps::Region> observed) {
  auto registry = std::make_shared<ps::OperationRegistry>();
  ps::OperationTraits source_traits;
  source_traits.output_element_type = ps::ElementType::Float64;
  source_traits.shape_rule = ps::OperationShapeRule::Fixed;
  source_traits.fixed_output_shape = {10U};
  source_traits.region_rule = ps::OperationRegionRule::Whole;
  ps::Status status = registry->register_operation(ps::OperationDefinition{
      "test.region_source", source_traits,
      [](const ps::OperationInvocation&) -> ps::Result<ps::Value> {
        return ps::Result<ps::Value>(vector_value(10U));
      }});
  if (!status.ok()) {
    throw std::logic_error(status.message);
  }
  ps::OperationTraits consumer_traits;
  consumer_traits.input_count = 1U;
  consumer_traits.output_element_type = ps::ElementType::Float64;
  consumer_traits.shape_rule = ps::OperationShapeRule::PreserveFirstInput;
  consumer_traits.region_rule = rule;
  consumer_traits.halo_radius = halo_radius;
  status = registry->register_operation(ps::OperationDefinition{
      "test.region", consumer_traits,
      [observed](
          const ps::OperationInvocation& invocation) -> ps::Result<ps::Value> {
        if (!observed || invocation.input_demands.size() != 1U) {
          return ps::Result<ps::Value>(ps::Status::failure(
              ps::ErrorCode::InvalidArgument,
              "region fixture did not receive one input demand"));
        }
        *observed = invocation.input_demands.front();
        return ps::Result<ps::Value>(invocation.inputs.front());
      }});
  if (!status.ok()) {
    throw std::logic_error(status.message);
  }
  registry->freeze();
  return registry;
}

/**
 * @brief Builds the common fixed-vector source and Region consumer workflow.
 * @return Two-node document with named output `value`.
 * @throws std::bad_alloc If source storage allocation fails.
 */
ps::WorkflowDocument region_document() {
  ps::WorkflowDocument document;
  document.nodes = {
      ps::WorkflowNode{1U, "test.region_source", {}, {}},
      ps::WorkflowNode{2U, "test.region", {ps::WorkflowInput{1U, "value"}}, {}},
  };
  document.outputs = {ps::WorkflowOutput{"value", 2U, "value"}};
  return document;
}

/** @brief Number of completed-compilation hook invocations in this test. */
std::uint32_t g_compilation_diagnostics_hook_calls = 0U;

/** @brief Number of completed-execution hook invocations in this test. */
std::uint32_t g_execution_diagnostics_hook_calls = 0U;

/**
 * @brief Returns legal deterministic compiler timing sentinels.
 * @return Immutable diagnostics with one legitimate zero-microsecond stage.
 * @throws Nothing.
 * @note Zero is valid because a stage may complete within clock resolution.
 */
const ps::CompilationDiagnostics& sentinel_compilation_diagnostics() noexcept {
  static const ps::CompilationDiagnostics diagnostics{0U, 11U, 13U};
  return diagnostics;
}

/**
 * @brief Builds internally coherent successful execution sentinels.
 * @return Diagnostics containing fallback, transfer, backend, timing, and
 * digest observations distinct from defaults.
 * @throws std::bad_alloc If sentinel container allocation fails.
 * @note The unavailable GPU attempt legitimately has zero duration and the
 * later CPU attempt is the selected backend for that node.
 */
ps::ExecutionDiagnostics make_sentinel_execution_diagnostics() {
  ps::ExecutionDiagnostics diagnostics;
  diagnostics.execute_us = 29U;
  diagnostics.selected_backends = {
      {1U, ps::Backend::Cpu},
      {2U, ps::Backend::Gpu},
      {3U, ps::Backend::Cpu},
  };
  diagnostics.transfer_count = 1U;
  diagnostics.transfer_bytes = sizeof(double);
  diagnostics.peak_live_bytes = 24U;
  diagnostics.fallback_reasons = {"node 1 sentinel GPU fallback"};
  diagnostics.operation_timings = {
      {1U, ps::Backend::Gpu, 0U, ps::ErrorCode::BackendUnavailable},
      {1U, ps::Backend::Cpu, 3U, ps::ErrorCode::Ok},
      {2U, ps::Backend::Gpu, 5U, ps::ErrorCode::Ok},
      {3U, ps::Backend::Cpu, 7U, ps::ErrorCode::Ok},
  };
  diagnostics.plan_digest = std::string(64U, 'a');
  diagnostics.result_digest = std::string(64U, 'b');
  return diagnostics;
}

/**
 * @brief Returns legal deterministic execution diagnostic sentinels.
 * @return Immutable diagnostics shared by both oracle-failure paths.
 * @throws std::bad_alloc On first-call sentinel construction failure.
 * @note The returned object remains alive until process shutdown.
 */
const ps::ExecutionDiagnostics& sentinel_execution_diagnostics() {
  static const ps::ExecutionDiagnostics diagnostics =
      make_sentinel_execution_diagnostics();
  return diagnostics;
}

/**
 * @brief Replaces completed compiler diagnostics with deterministic sentinels.
 * @param diagnostics Mutable successful compiler output.
 * @return No value.
 * @throws Nothing.
 * @note The call count proves that the test seam executed before sample copy.
 */
void inject_compilation_diagnostics(
    ps::CompilationDiagnostics& diagnostics) noexcept {
  ++g_compilation_diagnostics_hook_calls;
  diagnostics = sentinel_compilation_diagnostics();
}

/**
 * @brief Replaces completed executor diagnostics with deterministic sentinels.
 * @param diagnostics Mutable successful executor output.
 * @return No value.
 * @throws std::bad_alloc If copying sentinel containers fails.
 * @note The call count proves that the test seam executed before sample copy.
 */
void inject_execution_diagnostics(ps::ExecutionDiagnostics& diagnostics) {
  ++g_execution_diagnostics_hook_calls;
  diagnostics = sentinel_execution_diagnostics();
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
  using ps::Backend;
  using ps::CancellationSource;
  using ps::CompiledWorkflow;
  using ps::Compiler;
  using ps::CorrectnessObservation;
  using ps::ElementType;
  using ps::ErrorCode;
  using ps::ExecutionContext;
  using ps::ExecutionContextConfig;
  using ps::ExecutionOptions;
  using ps::ExecutionResult;
  using ps::GraphContext;
  using ps::make_default_operation_registry;
  using ps::OperationDefinition;
  using ps::OperationInvocation;
  using ps::OperationRegionRule;
  using ps::OperationRegistry;
  using ps::OperationShapeRule;
  using ps::OperationTraits;
  using ps::PlanningOptions;
  using ps::RawBenchmarkOptions;
  using ps::RawBenchmarkRunner;
  using ps::RawBenchmarkSample;
  using ps::Region;
  using ps::RegionDimension;
  using ps::Result;
  using ps::StridedLayout;
  using ps::Value;
  using ps::ValueDescriptor;
  using ps::ValueFacet;
  using ps::WorkflowDocument;
  using ps::WorkflowInput;
  using ps::WorkflowNode;
  using ps::WorkflowOutput;

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

  std::mutex effect_mutex;
  std::uint64_t source_calls = 0U;
  std::uint64_t sink_calls = 0U;
  std::vector<std::uint64_t> effect_order;
  auto effect_operations = std::make_shared<OperationRegistry>();
  OperationTraits effect_source_traits;
  effect_source_traits.deterministic = true;
  effect_source_traits.side_effect_free = false;
  effect_source_traits.cacheable = false;
  PS_CHECK(effect_operations
               ->register_operation(OperationDefinition{
                   "test.effect_source", effect_source_traits,
                   [&effect_mutex, &source_calls, &effect_order](
                       const OperationInvocation&) -> Result<Value> {
                     double value = 0.0;
                     {
                       std::lock_guard<std::mutex> lock(effect_mutex);
                       ++source_calls;
                       effect_order.push_back(1U);
                       value = static_cast<double>(source_calls);
                     }
                     return Result<Value>(Value::from_float64(value));
                   }})
               .ok());
  OperationTraits effect_sink_traits;
  effect_sink_traits.input_count = 1U;
  effect_sink_traits.deterministic = true;
  effect_sink_traits.side_effect_free = false;
  effect_sink_traits.cacheable = false;
  effect_sink_traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  PS_CHECK(effect_operations
               ->register_operation(OperationDefinition{
                   "test.effect_sink", effect_sink_traits,
                   [&effect_mutex, &sink_calls, &effect_order](
                       const OperationInvocation& invocation) -> Result<Value> {
                     if (invocation.inputs.size() != 1U) {
                       return Result<Value>(ps::Status::failure(
                           ErrorCode::InvalidArgument,
                           "effect sink requires one input"));
                     }
                     {
                       std::lock_guard<std::mutex> lock(effect_mutex);
                       ++sink_calls;
                       effect_order.push_back(2U);
                     }
                     return Result<Value>(invocation.inputs.front());
                   }})
               .ok());
  effect_operations->freeze();
  Compiler effect_compiler(effect_operations);
  ExecutionContext effect_execution(
      effect_operations, ExecutionContextConfig{1U, false, 4U, 1024U});
  WorkflowDocument effect_document;
  effect_document.nodes = {
      WorkflowNode{1U, "test.effect_source", {}, {}},
      WorkflowNode{2U, "test.effect_sink", {WorkflowInput{1U, "value"}}, {}},
  };
  effect_document.outputs = {WorkflowOutput{"effect", 2U, "value"}};
  GraphContext effect_graph(std::move(effect_document));
  CompiledWorkflow effect_workflow =
      compile_or_throw(&effect_compiler, effect_graph);
  CompiledWorkflow repeated_effect_workflow =
      compile_or_throw(&effect_compiler, effect_graph);
  PS_CHECK(effect_workflow.semantic.nodes().size() == 2U);
  PS_CHECK(effect_workflow.optimized.nodes().size() == 2U);
  PS_CHECK(effect_workflow.plan.steps().size() == 2U);
  PS_CHECK(effect_workflow.semantic.nodes()[0U].operation ==
           "test.effect_source");
  PS_CHECK(effect_workflow.semantic.nodes()[1U].operation ==
           "test.effect_sink");
  for (std::size_t index = 0U; index < 2U; ++index) {
    const auto& semantic_node = effect_workflow.semantic.nodes()[index];
    const auto& optimized_node = effect_workflow.optimized.nodes()[index];
    const auto& plan_step = effect_workflow.plan.steps()[index];
    PS_CHECK(semantic_node.id == index + 1U);
    PS_CHECK(optimized_node.id == semantic_node.id);
    PS_CHECK(plan_step.node_id == semantic_node.id);
    PS_CHECK(optimized_node.operation == semantic_node.operation);
    PS_CHECK(plan_step.operation == semantic_node.operation);
    PS_CHECK(semantic_node.traits.deterministic);
    PS_CHECK(!semantic_node.traits.side_effect_free);
    PS_CHECK(!semantic_node.traits.cacheable);
    PS_CHECK(!optimized_node.traits.side_effect_free);
    PS_CHECK(!optimized_node.traits.cacheable);
    PS_CHECK(!plan_step.traits.side_effect_free);
    PS_CHECK(!plan_step.traits.cacheable);
  }
  PS_CHECK(effect_workflow.semantic.nodes()[1U].inputs ==
           std::vector<std::uint64_t>({1U}));
  PS_CHECK(effect_workflow.optimized.nodes()[1U].inputs ==
           std::vector<std::uint64_t>({1U}));
  PS_CHECK(effect_workflow.plan.steps()[1U].input_steps ==
           std::vector<std::size_t>({0U}));
  PS_CHECK(effect_workflow.optimized.semantic_digest().value ==
           effect_workflow.semantic.digest().value);
  PS_CHECK(effect_workflow.plan.optimized_digest().value ==
           effect_workflow.optimized.digest().value);
  PS_CHECK(!effect_workflow.plan.digest().value.empty());
  PS_CHECK(!effect_workflow.plan.cache_key().value.empty());
  PS_CHECK(effect_workflow.plan.digest().value !=
           effect_workflow.plan.cache_key().value);
  PS_CHECK(repeated_effect_workflow.semantic.digest().value ==
           effect_workflow.semantic.digest().value);
  PS_CHECK(repeated_effect_workflow.optimized.digest().value ==
           effect_workflow.optimized.digest().value);
  PS_CHECK(repeated_effect_workflow.plan.digest().value ==
           effect_workflow.plan.digest().value);
  PS_CHECK(repeated_effect_workflow.plan.cache_key().value ==
           effect_workflow.plan.cache_key().value);
  ExecutionOptions serial_execution;
  serial_execution.maximum_parallelism = 1U;
  auto first_effect_result =
      effect_execution.execute(effect_workflow.plan, {}, serial_execution);
  auto second_effect_result =
      effect_execution.execute(effect_workflow.plan, {}, serial_execution);
  PS_CHECK(first_effect_result.ok());
  PS_CHECK(second_effect_result.ok());
  PS_CHECK(ps::test::named_scalar(first_effect_result.value(), "effect") ==
           1.0);
  PS_CHECK(ps::test::named_scalar(second_effect_result.value(), "effect") ==
           2.0);
  PS_CHECK(first_effect_result.value().diagnostics.operation_timings.size() ==
           2U);
  PS_CHECK(second_effect_result.value().diagnostics.operation_timings.size() ==
           2U);
  PS_CHECK(first_effect_result.value().diagnostics.plan_digest ==
           effect_workflow.plan.digest().value);
  PS_CHECK(second_effect_result.value().diagnostics.plan_digest ==
           effect_workflow.plan.digest().value);
  {
    std::lock_guard<std::mutex> lock(effect_mutex);
    PS_CHECK(source_calls == 2U);
    PS_CHECK(sink_calls == 2U);
    PS_CHECK(effect_order == std::vector<std::uint64_t>({1U, 2U, 1U, 2U}));
  }

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
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  PS_CHECK(cancellation.cancel());
  auto cancelled_result = cancelled_future.get();
  PS_CHECK(!cancelled_result.ok());
  PS_CHECK(cancelled_result.status().code == ErrorCode::Cancelled);

  GraphContext replaceable(ps::test::delayed_document(120));
  CompiledWorkflow stale_workflow = compile_or_throw(&compiler, replaceable);
  auto stale_future = std::async(std::launch::async, [&] {
    return execution.execute(stale_workflow.plan);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
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

  auto whole_observed = std::make_shared<Region>();
  auto exact_observed = std::make_shared<Region>();
  auto halo_observed = std::make_shared<Region>();
  auto whole_operations =
      make_region_registry(OperationRegionRule::Whole, 0U, whole_observed);
  auto exact_operations = make_region_registry(OperationRegionRule::Elementwise,
                                               0U, exact_observed);
  auto halo_operations =
      make_region_registry(OperationRegionRule::Halo, 2U, halo_observed);
  Compiler whole_compiler(whole_operations);
  Compiler exact_compiler(exact_operations);
  Compiler halo_compiler(halo_operations);
  GraphContext whole_graph(region_document());
  GraphContext exact_graph(region_document());
  GraphContext halo_graph(region_document());
  PlanningOptions region_options;
  region_options.output_regions.emplace("value",
                                        Region({RegionDimension{4U, 2U}}));
  auto whole_workflow = whole_compiler.compile(whole_graph, region_options);
  auto exact_workflow = exact_compiler.compile(exact_graph, region_options);
  auto halo_workflow = halo_compiler.compile(halo_graph, region_options);
  PS_CHECK(whole_workflow.ok());
  PS_CHECK(exact_workflow.ok());
  PS_CHECK(halo_workflow.ok());
  PS_CHECK(region_equals(
      whole_workflow.value().plan.steps().back().input_demands.front(), 0U,
      10U));
  PS_CHECK(region_equals(
      exact_workflow.value().plan.steps().back().input_demands.front(), 4U,
      2U));
  PS_CHECK(region_equals(
      halo_workflow.value().plan.steps().back().input_demands.front(), 2U, 6U));
  PS_CHECK(whole_workflow.value().plan.digest().value !=
           exact_workflow.value().plan.digest().value);
  PS_CHECK(exact_workflow.value().plan.digest().value !=
           halo_workflow.value().plan.digest().value);
  ExecutionContext whole_execution(whole_operations);
  ExecutionContext exact_execution(exact_operations);
  ExecutionContext halo_execution(halo_operations);
  PS_CHECK(whole_execution.execute(whole_workflow.value().plan).ok());
  PS_CHECK(exact_execution.execute(exact_workflow.value().plan).ok());
  PS_CHECK(halo_execution.execute(halo_workflow.value().plan).ok());
  PS_CHECK(region_equals(*whole_observed, 0U, 10U));
  PS_CHECK(region_equals(*exact_observed, 4U, 2U));
  PS_CHECK(region_equals(*halo_observed, 2U, 6U));

  auto clipped_observed = std::make_shared<Region>();
  auto clipped_operations = make_region_registry(
      OperationRegionRule::Halo, std::numeric_limits<std::uint32_t>::max(),
      clipped_observed);
  Compiler clipped_compiler(clipped_operations);
  GraphContext clipped_graph(region_document());
  auto clipped_workflow =
      clipped_compiler.compile(clipped_graph, region_options);
  PS_CHECK(clipped_workflow.ok());
  PS_CHECK(region_equals(
      clipped_workflow.value().plan.steps().back().input_demands.front(), 0U,
      10U));
  ExecutionContext clipped_execution(clipped_operations);
  PS_CHECK(clipped_execution.execute(clipped_workflow.value().plan).ok());
  PS_CHECK(region_equals(*clipped_observed, 0U, 10U));

  PlanningOptions unknown_region_options;
  unknown_region_options.output_regions.emplace(
      "missing", Region({RegionDimension{0U, 1U}}));
  PS_CHECK(!exact_compiler.compile(exact_graph, unknown_region_options).ok());

  auto resource_operations = std::make_shared<OperationRegistry>();
  OperationTraits large_traits;
  large_traits.supports_gpu = false;
  large_traits.allows_cpu_fallback = false;
  large_traits.estimated_bytes = 16U;
  PS_CHECK(resource_operations
               ->register_operation(OperationDefinition{
                   "test.large", large_traits,
                   [](const OperationInvocation&) -> Result<Value> {
                     return Result<Value>(Value::from_float64(1.0));
                   }})
               .ok());
  OperationTraits small_traits = large_traits;
  small_traits.estimated_bytes = 8U;
  PS_CHECK(resource_operations
               ->register_operation(OperationDefinition{
                   "test.small", small_traits,
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
  benchmark_options.oracle_name = "sum-equals-6.5";
  auto report = benchmark.run(graph, benchmark_options);
  PS_CHECK(report.ok());
  PS_CHECK(report.value().oracle_name == "sum-equals-6.5");
  PS_CHECK(report.value().samples.size() == 2U);
  for (const RawBenchmarkSample& sample : report.value().samples) {
    PS_CHECK(sample.outcome == ErrorCode::Ok);
    PS_CHECK(sample.correctness_checked);
    PS_CHECK(sample.correctness_accepted);
    PS_CHECK(sample.oracle_name == "sum-equals-6.5");
    PS_CHECK(!sample.execution.plan_digest.empty());
  }

  RawBenchmarkOptions rejected_oracle_options;
  rejected_oracle_options.execution.maximum_parallelism = 1U;
  rejected_oracle_options.correctness_oracle =
      [](const ExecutionResult&) -> CorrectnessObservation {
    return CorrectnessObservation{false, "intentional mismatch"};
  };
  rejected_oracle_options.oracle_name = "always-rejects";
  const auto& expected_compilation = sentinel_compilation_diagnostics();
  const auto& expected_execution = sentinel_execution_diagnostics();
  const ps::benchmark_testing::RawBenchmarkTestHooks benchmark_test_hooks{
      &inject_compilation_diagnostics,
      &inject_execution_diagnostics,
  };
  ps::benchmark_testing::install_raw_benchmark_test_hooks(
      &benchmark_test_hooks);
  auto rejected_oracle_report = benchmark.run(graph, rejected_oracle_options);
  PS_CHECK(rejected_oracle_report.ok());
  PS_CHECK(g_compilation_diagnostics_hook_calls == 1U);
  PS_CHECK(g_execution_diagnostics_hook_calls == 1U);
  PS_CHECK(rejected_oracle_report.value().oracle_name == "always-rejects");
  PS_CHECK(rejected_oracle_report.value().samples.size() == 1U);
  const RawBenchmarkSample& rejected_sample =
      rejected_oracle_report.value().samples.front();
  PS_CHECK(rejected_sample.iteration == 0U);
  PS_CHECK(rejected_sample.oracle_name == "always-rejects");
  PS_CHECK(rejected_sample.correctness_checked);
  PS_CHECK(!rejected_sample.correctness_accepted);
  PS_CHECK(rejected_sample.outcome == ErrorCode::OperationFailed);
  PS_CHECK(rejected_sample.reason == "intentional mismatch");
  PS_CHECK(rejected_sample.compilation.analyze_us ==
           expected_compilation.analyze_us);
  PS_CHECK(rejected_sample.compilation.optimize_us ==
           expected_compilation.optimize_us);
  PS_CHECK(rejected_sample.compilation.plan_us == expected_compilation.plan_us);
  PS_CHECK(rejected_sample.execution.execute_us ==
           expected_execution.execute_us);
  PS_CHECK(rejected_sample.execution.selected_backends ==
           expected_execution.selected_backends);
  PS_CHECK(rejected_sample.execution.transfer_count ==
           expected_execution.transfer_count);
  PS_CHECK(rejected_sample.execution.transfer_bytes ==
           expected_execution.transfer_bytes);
  PS_CHECK(rejected_sample.execution.peak_live_bytes ==
           expected_execution.peak_live_bytes);
  PS_CHECK(rejected_sample.execution.fallback_reasons ==
           expected_execution.fallback_reasons);
  PS_CHECK(rejected_sample.execution.plan_digest ==
           expected_execution.plan_digest);
  PS_CHECK(rejected_sample.execution.result_digest ==
           expected_execution.result_digest);
  PS_CHECK(rejected_sample.execution.operation_timings.size() ==
           expected_execution.operation_timings.size());
  for (std::size_t index = 0U;
       index < rejected_sample.execution.operation_timings.size(); ++index) {
    const auto& actual = rejected_sample.execution.operation_timings[index];
    const auto& expected = expected_execution.operation_timings[index];
    PS_CHECK(actual.node_id == expected.node_id);
    PS_CHECK(actual.backend == expected.backend);
    PS_CHECK(actual.duration_us == expected.duration_us);
    PS_CHECK(actual.outcome == expected.outcome);
  }

  RawBenchmarkOptions throwing_oracle_options = rejected_oracle_options;
  throwing_oracle_options.correctness_oracle =
      [](const ExecutionResult&) -> CorrectnessObservation {
    throw std::runtime_error("intentional oracle exception");
  };
  throwing_oracle_options.oracle_name = "always-throws";
  auto throwing_oracle_report = benchmark.run(graph, throwing_oracle_options);
  PS_CHECK(throwing_oracle_report.ok());
  PS_CHECK(g_compilation_diagnostics_hook_calls == 2U);
  PS_CHECK(g_execution_diagnostics_hook_calls == 2U);
  PS_CHECK(throwing_oracle_report.value().oracle_name == "always-throws");
  PS_CHECK(throwing_oracle_report.value().samples.size() == 1U);
  const RawBenchmarkSample& throwing_sample =
      throwing_oracle_report.value().samples.front();
  PS_CHECK(throwing_sample.iteration == 0U);
  PS_CHECK(throwing_sample.oracle_name == "always-throws");
  PS_CHECK(throwing_sample.correctness_checked);
  PS_CHECK(!throwing_sample.correctness_accepted);
  PS_CHECK(throwing_sample.outcome == ErrorCode::OperationFailed);
  PS_CHECK(throwing_sample.reason == "intentional oracle exception");
  PS_CHECK(throwing_sample.compilation.analyze_us ==
           expected_compilation.analyze_us);
  PS_CHECK(throwing_sample.compilation.optimize_us ==
           expected_compilation.optimize_us);
  PS_CHECK(throwing_sample.compilation.plan_us == expected_compilation.plan_us);
  PS_CHECK(throwing_sample.execution.execute_us ==
           expected_execution.execute_us);
  PS_CHECK(throwing_sample.execution.selected_backends ==
           expected_execution.selected_backends);
  PS_CHECK(throwing_sample.execution.transfer_count ==
           expected_execution.transfer_count);
  PS_CHECK(throwing_sample.execution.transfer_bytes ==
           expected_execution.transfer_bytes);
  PS_CHECK(throwing_sample.execution.peak_live_bytes ==
           expected_execution.peak_live_bytes);
  PS_CHECK(throwing_sample.execution.fallback_reasons ==
           expected_execution.fallback_reasons);
  PS_CHECK(throwing_sample.execution.plan_digest ==
           expected_execution.plan_digest);
  PS_CHECK(throwing_sample.execution.result_digest ==
           expected_execution.result_digest);
  PS_CHECK(throwing_sample.execution.operation_timings.size() ==
           expected_execution.operation_timings.size());
  for (std::size_t index = 0U;
       index < throwing_sample.execution.operation_timings.size(); ++index) {
    const auto& actual = throwing_sample.execution.operation_timings[index];
    const auto& expected = expected_execution.operation_timings[index];
    PS_CHECK(actual.node_id == expected.node_id);
    PS_CHECK(actual.backend == expected.backend);
    PS_CHECK(actual.duration_us == expected.duration_us);
    PS_CHECK(actual.outcome == expected.outcome);
  }
  ps::benchmark_testing::install_raw_benchmark_test_hooks(nullptr);

  RawBenchmarkOptions unchecked_options;
  auto unchecked_report = benchmark.run(graph, unchecked_options);
  PS_CHECK(unchecked_report.ok());
  PS_CHECK(unchecked_report.value().oracle_name == "unchecked");
  PS_CHECK(unchecked_report.value().samples.size() == 1U);
  PS_CHECK(!unchecked_report.value().samples.front().correctness_checked);
  PS_CHECK(!unchecked_report.value().samples.front().correctness_accepted);
  PS_CHECK(unchecked_report.value().samples.front().oracle_name == "unchecked");

  RawBenchmarkOptions missing_oracle_name = benchmark_options;
  missing_oracle_name.oracle_name.clear();
  auto missing_oracle_name_result = benchmark.run(graph, missing_oracle_name);
  PS_CHECK(!missing_oracle_name_result.ok());
  PS_CHECK(missing_oracle_name_result.status().code ==
           ErrorCode::InvalidArgument);

  RawBenchmarkOptions reserved_oracle_name = benchmark_options;
  reserved_oracle_name.oracle_name = "unchecked";
  auto reserved_oracle_name_result = benchmark.run(graph, reserved_oracle_name);
  PS_CHECK(!reserved_oracle_name_result.ok());
  PS_CHECK(reserved_oracle_name_result.status().code ==
           ErrorCode::InvalidArgument);

  RawBenchmarkOptions conflicting_oracle_name;
  conflicting_oracle_name.oracle_name = "named-but-unchecked";
  auto conflicting_oracle_name_result =
      benchmark.run(graph, conflicting_oracle_name);
  PS_CHECK(!conflicting_oracle_name_result.ok());
  PS_CHECK(conflicting_oracle_name_result.status().code ==
           ErrorCode::InvalidArgument);

  RawBenchmarkOptions invalid_utf8_oracle = benchmark_options;
  invalid_utf8_oracle.oracle_name = std::string("bad\xc3\x28", 5U);
  auto invalid_utf8_oracle_result = benchmark.run(graph, invalid_utf8_oracle);
  PS_CHECK(!invalid_utf8_oracle_result.ok());
  PS_CHECK(invalid_utf8_oracle_result.status().code ==
           ErrorCode::InvalidArgument);

  RawBenchmarkOptions oversized_oracle = benchmark_options;
  oversized_oracle.oracle_name.assign(129U, 'x');
  auto oversized_oracle_result = benchmark.run(graph, oversized_oracle);
  PS_CHECK(!oversized_oracle_result.ok());
  PS_CHECK(oversized_oracle_result.status().code == ErrorCode::InvalidArgument);
  return 0;
}
