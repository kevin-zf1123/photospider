#include <string>
#include <utility>

#include "photospider/compiler/compiler.hpp"
#include "support/test_support.hpp"

/**
 * @brief Exercises typed stage identity, validation, ordering, and staleness.
 * @return Zero when all checks pass.
 * @throws std::bad_alloc If test setup allocation fails.
 * @note Behavioral failures otherwise return nonzero through `PS_CHECK`.
 */
int main() {
  using namespace ps;

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
