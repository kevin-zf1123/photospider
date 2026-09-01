#include <cmath>
#include <memory>
#include <string>
#include <utility>

#include "photospider/compiler/compiler.hpp"
#include "support/test_support.hpp"

/**
 * @brief Exercises typed stage identity, exact parameter bits, validation,
 * ordering, execution visibility, and staleness.
 * @return Zero when all checks pass.
 * @throws std::bad_alloc If test setup allocation fails.
 * @note Behavioral failures otherwise return nonzero through `PS_CHECK`.
 */
int main() {
  using ps::Backend;
  using ps::Compiler;
  using ps::ErrorCode;
  using ps::GraphContext;
  using ps::GraphSnapshot;
  using ps::make_default_operation_registry;
  using ps::PlanningOptions;
  using ps::WorkflowDocument;
  using ps::WorkflowInput;
  using ps::WorkflowNode;
  using ps::WorkflowOutput;

  auto operations = make_default_operation_registry();
  Compiler compiler(operations);
  GraphContext first(ps::test::addition_document(2.0, 3.0));
  auto compiled = compiler.compile(first);
  PS_CHECK(compiled.ok());
  PS_CHECK(compiled.value().semantic.nodes().size() == 3U);
  PS_CHECK(compiled.value().semantic.digest().value.size() == 16U);
  PS_CHECK(compiled.value().optimized.digest().value.size() == 16U);
  PS_CHECK(compiled.value().plan.digest().value.size() == 16U);
  PS_CHECK(compiled.value().plan.cache_key().value.size() == 16U);
  PS_CHECK(compiled.value().semantic.digest().value !=
           compiled.value().optimized.digest().value);
  PS_CHECK(compiled.value().optimized.digest().value !=
           compiled.value().plan.digest().value);
  PS_CHECK(compiled.value().plan.digest().value !=
           compiled.value().plan.cache_key().value);
  PS_CHECK(compiled.value().plan.steps().back().backend == Backend::Cpu);

  WorkflowDocument unknown_parameter = ps::test::addition_document(2.0, 3.0);
  unknown_parameter.nodes.front().parameters = {{"vlaue", 2.0}};
  GraphContext unknown_parameter_context(std::move(unknown_parameter));
  auto unknown_parameter_result = compiler.compile(unknown_parameter_context);
  PS_CHECK(!unknown_parameter_result.ok());
  PS_CHECK(unknown_parameter_result.status().code ==
           ErrorCode::InvalidArgument);

  WorkflowDocument missing_parameter = ps::test::addition_document(2.0, 3.0);
  missing_parameter.nodes.front().parameters.clear();
  GraphContext missing_parameter_context(std::move(missing_parameter));
  auto missing_parameter_result = compiler.compile(missing_parameter_context);
  PS_CHECK(!missing_parameter_result.ok());
  PS_CHECK(missing_parameter_result.status().code ==
           ErrorCode::InvalidArgument);

  WorkflowDocument wrong_parameter_type = ps::test::addition_document(2.0, 3.0);
  wrong_parameter_type.nodes.front().parameters = {{"value", true}};
  GraphContext wrong_parameter_type_context(std::move(wrong_parameter_type));
  auto wrong_parameter_type_result =
      compiler.compile(wrong_parameter_type_context);
  PS_CHECK(!wrong_parameter_type_result.ok());
  PS_CHECK(wrong_parameter_type_result.status().code ==
           ErrorCode::InvalidArgument);

  GraphContext equivalent(ps::test::addition_document(2.0, 3.0));
  auto equivalent_compiled = compiler.compile(equivalent);
  PS_CHECK(equivalent_compiled.ok());
  PS_CHECK(compiled.value().semantic.digest().value ==
           equivalent_compiled.value().semantic.digest().value);

  auto signed_zero_operations = std::make_shared<ps::OperationRegistry>();
  ps::OperationTraits signed_zero_traits;
  signed_zero_traits.parameter_schema = {ps::OperationParameterSpec{
      "value", ps::OperationParameterType::Float64, true}};
  PS_CHECK(signed_zero_operations
               ->register_operation(ps::OperationDefinition{
                   "test.signed_zero", signed_zero_traits,
                   [](const ps::OperationInvocation& invocation)
                       -> ps::Result<ps::Value> {
                     const auto parameter = invocation.parameters.find("value");
                     if (parameter == invocation.parameters.end()) {
                       return ps::Result<ps::Value>(ps::Status::failure(
                           ErrorCode::InvalidArgument,
                           "signed-zero probe parameter is missing"));
                     }
                     const auto* value =
                         std::get_if<double>(&parameter->second);
                     if (!value) {
                       return ps::Result<ps::Value>(ps::Status::failure(
                           ErrorCode::InvalidArgument,
                           "signed-zero probe parameter is not Float64"));
                     }
                     return ps::Result<ps::Value>(ps::Value::from_float64(
                         std::signbit(*value) ? -1.0 : 1.0));
                   }})
               .ok());
  PS_CHECK(signed_zero_operations->freeze().ok());
  Compiler signed_zero_compiler(signed_zero_operations);
  WorkflowDocument positive_zero_document;
  positive_zero_document.nodes = {
      WorkflowNode{1U, "test.signed_zero", {}, {{"value", 0.0}}}};
  positive_zero_document.outputs = {WorkflowOutput{"value", 1U, "value"}};
  WorkflowDocument negative_zero_document = positive_zero_document;
  negative_zero_document.nodes.front().parameters = {{"value", -0.0}};
  GraphContext positive_zero_graph(std::move(positive_zero_document));
  GraphContext negative_zero_graph(std::move(negative_zero_document));
  auto positive_zero = signed_zero_compiler.compile(positive_zero_graph);
  auto negative_zero = signed_zero_compiler.compile(negative_zero_graph);
  PS_CHECK(positive_zero.ok());
  PS_CHECK(negative_zero.ok());
  PS_CHECK(positive_zero.value().semantic.digest().value == "7ad5d0e23f1164b3");
  PS_CHECK(positive_zero.value().optimized.digest().value ==
           "02d649acb07e120a");
  PS_CHECK(positive_zero.value().plan.digest().value == "b3201be660702a9c");
  PS_CHECK(positive_zero.value().plan.cache_key().value == "61853ba3f4ca3fa0");
  PS_CHECK(negative_zero.value().semantic.digest().value == "e5682637a9eab433");
  PS_CHECK(negative_zero.value().optimized.digest().value ==
           "0d4103d0a4a6dcce");
  PS_CHECK(negative_zero.value().plan.digest().value == "edee134b3dc08cfc");
  PS_CHECK(negative_zero.value().plan.cache_key().value == "cd7b604b0b72766e");
  PS_CHECK(positive_zero.value().semantic.digest().value !=
           negative_zero.value().semantic.digest().value);
  PS_CHECK(positive_zero.value().optimized.digest().value !=
           negative_zero.value().optimized.digest().value);
  PS_CHECK(positive_zero.value().plan.digest().value !=
           negative_zero.value().plan.digest().value);
  PS_CHECK(positive_zero.value().plan.cache_key().value !=
           negative_zero.value().plan.cache_key().value);
  ps::ExecutionContext signed_zero_execution(signed_zero_operations);
  auto positive_zero_result =
      signed_zero_execution.execute(positive_zero.value().plan);
  auto negative_zero_result =
      signed_zero_execution.execute(negative_zero.value().plan);
  PS_CHECK(positive_zero_result.ok());
  PS_CHECK(negative_zero_result.ok());
  PS_CHECK(ps::test::named_scalar(positive_zero_result.value(), "value") ==
           1.0);
  PS_CHECK(ps::test::named_scalar(negative_zero_result.value(), "value") ==
           -1.0);

  WorkflowDocument duplicate = ps::test::addition_document(1.0, 1.0);
  duplicate.nodes[1].id = 1U;
  GraphContext duplicate_context(std::move(duplicate));
  auto duplicate_result = compiler.compile(duplicate_context);
  PS_CHECK(!duplicate_result.ok());
  PS_CHECK(duplicate_result.status().code == ErrorCode::InvalidArgument);

  WorkflowDocument missing = ps::test::addition_document(1.0, 1.0);
  missing.nodes.back().inputs.back().source_node = 99U;
  GraphContext missing_context(std::move(missing));
  auto missing_result = compiler.compile(missing_context);
  PS_CHECK(!missing_result.ok());
  PS_CHECK(missing_result.status().code == ErrorCode::NotFound);

  WorkflowDocument cycle;
  cycle.nodes = {
      WorkflowNode{1U, "core.identity", {WorkflowInput{2U, "value"}}, {}},
      WorkflowNode{2U, "core.identity", {WorkflowInput{1U, "value"}}, {}},
  };
  cycle.outputs = {WorkflowOutput{"cycle", 1U, "value"}};
  GraphContext cycle_context(std::move(cycle));
  auto cycle_result = compiler.compile(cycle_context);
  PS_CHECK(!cycle_result.ok());
  PS_CHECK(cycle_result.status().code == ErrorCode::Cycle);

  GraphContext mutable_context(ps::test::addition_document(4.0, 5.0));
  GraphSnapshot stale_snapshot = mutable_context.snapshot();
  mutable_context.replace(ps::test::addition_document(6.0, 7.0));
  auto stale_result = compiler.analyze(stale_snapshot);
  PS_CHECK(!stale_result.ok());
  PS_CHECK(stale_result.status().code == ErrorCode::Stale);

  PlanningOptions gpu_options;
  gpu_options.allow_gpu = true;
  auto gpu_compiled = compiler.compile(first, gpu_options);
  PS_CHECK(gpu_compiled.ok());
  PS_CHECK(gpu_compiled.value().plan.steps().front().backend == Backend::Gpu);
  PS_CHECK(gpu_compiled.value().plan.digest().value !=
           compiled.value().plan.digest().value);

  auto other_operations = make_default_operation_registry();
  Compiler other_compiler(other_operations);
  auto mismatched_optimization =
      other_compiler.optimize(compiled.value().semantic);
  PS_CHECK(!mismatched_optimization.ok());
  PS_CHECK(mismatched_optimization.status().code == ErrorCode::Stale);
  return 0;
}
