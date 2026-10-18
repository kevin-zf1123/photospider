#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/artifact.hpp"
#include "support/kernel_registry_fixture.hpp"
#include "support/test_support.hpp"

namespace {
template <class IR>
int check_chains(const IR& semantic,
                 const std::vector<std::vector<ps::IRCallId>>& expected) {
  const auto& blocks = semantic.blocks();
  PS_CHECK(blocks.size() == expected.size());
  PS_CHECK(semantic.entry() == blocks.front().id);
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    PS_CHECK(blocks[i].calls == expected[i]);
    PS_CHECK(blocks[i].parameters.empty());
    if (i + 1 < blocks.size()) {
      const auto* jump = std::get_if<ps::IRJump>(&blocks[i].terminator);
      PS_CHECK(jump && jump->edge.target == blocks[i + 1].id);
      PS_CHECK(jump->edge.arguments.empty());
    } else {
      PS_CHECK(std::holds_alternative<ps::IRReturn>(blocks[i].terminator));
    }
  }
  return 0;
}

int dependency_chains() {
  using ps::WorkflowNodeOutput;
  auto operations = ps::test::kernel_registry();
  ps::Compiler compiler(operations);
  // IDs deliberately disagree with dependency order: A=70, B=60, ... G=10.
  ps::WorkflowDocument document;
  document.nodes = {
      {70, "core.constant", {}, {{"value", 2.0}}},
      {60, "core.identity", {WorkflowNodeOutput{70, "value"}}, {}},
      {50, "core.identity", {WorkflowNodeOutput{70, "value"}}, {}},
      {40,
       "math.add",
       {WorkflowNodeOutput{60, "value"}, WorkflowNodeOutput{50, "value"}},
       {}},
      {30, "core.identity", {WorkflowNodeOutput{40, "value"}}, {}},
      {20,
       "math.add",
       {WorkflowNodeOutput{10, "value"}, WorkflowNodeOutput{30, "value"}},
       {}},
      {10, "core.identity", {WorkflowNodeOutput{60, "value"}}, {}}};
  document.outputs = {{"result", 20, "value"}};
  ps::GraphContext graph(document);
  auto compiled = compiler.compile(graph, {}, {}, {{"optimize", {}}});
  PS_REQUIRE_OK(compiled);
  PS_CHECK(check_chains(compiled.value().ir_versions.front().ir.value(),
                        {{70, 60, 10}, {70, 60}, {70, 50}, {40, 30}, {20}}) ==
           0);
  PS_CHECK(compiled.value().ir_versions.front().ir.value().program().form ==
           ps::IRForm::DependencyPaths);
  PS_CHECK(compiled.value().ir_versions.back().ir.value().program().form ==
           ps::IRForm::ControlFlow);
  PS_CHECK(check_chains(compiled.value().ir_versions.back().ir.value(),
                        {{70, 60, 10}, {50}, {40, 30}, {20}}) == 0);
  PS_CHECK(compiled.value().ir_versions.front().ir.value().nodes().size() == 7);
  std::vector<ps::IRCallId> executed;
  for (const auto& block : compiled.value().plan.value().blocks())
    for (const auto& instruction : block.instructions)
      executed.push_back(compiled.value()
                             .plan.value()
                             .steps()[instruction.step_index]
                             .node_id);
  PS_CHECK(executed == std::vector<ps::IRCallId>({70, 60, 10, 50, 40, 30, 20}));
  ps::ExecutionContext execution(operations);
  auto result = execution.execute(compiled.value().plan.value());
  PS_REQUIRE_OK(result);
  PS_CHECK(ps::test::named_scalar(result.value(), "result") == 6.0);
  auto encoded = ps::CompilerArtifact::export_ir(
      compiled.value().ir_versions.front().ir.value());
  PS_REQUIRE_OK(encoded);
  auto loaded = ps::CompilerArtifact::import_ir(compiler, encoded.value());
  PS_REQUIRE_OK(loaded);
  const auto& raw = loaded.value();
  PS_CHECK(raw.digest().value ==
           compiled.value().ir_versions.front().ir.value().digest().value);
  auto loaded_optimized = compiler.optimize(raw);
  PS_REQUIRE_OK(loaded_optimized);
  PS_CHECK(loaded_optimized.value().digest().value ==
           compiled.value().ir_versions.back().ir.value().digest().value);
  auto encoded_again = ps::CompilerArtifact::export_ir(raw);
  PS_REQUIRE_OK(encoded_again);
  PS_CHECK(encoded_again.value() == encoded.value());
  auto invalid_form = raw.program();
  invalid_form.form = ps::IRForm::ControlFlow;
  PS_CHECK(!compiler.bind_ir(invalid_form).ok());
  invalid_form = raw.program();
  invalid_form.blocks.front().terminator = ps::IRJump{{invalid_form.entry, {}}};
  PS_CHECK(!compiler.bind_ir(invalid_form).ok());
  auto wrong_stage = encoded.value();
  wrong_stage.replace(wrong_stage.find("stage ir"), 8, "stage invalid");
  PS_CHECK(!ps::CompilerArtifact::import_ir(compiler, wrong_stage).ok());
  invalid_form = raw.program();
  invalid_form.blocks.front().calls.assign(1048577, 70);
  PS_CHECK(compiler.bind_ir(invalid_form).status().code ==
           ps::ErrorCode::ResourceExhausted);

  // Document storage order cannot change traversal or compiled identities.
  std::reverse(document.nodes.begin(), document.nodes.end());
  ps::GraphContext reordered(document);
  auto same = compiler.compile(reordered, {}, {}, {{"optimize", {}}});
  PS_REQUIRE_OK(same);
  PS_CHECK(same.value().ir_versions.front().ir.value().digest().value ==
           compiled.value().ir_versions.front().ir.value().digest().value);
  PS_CHECK(same.value().plan.value().digest().value ==
           compiled.value().plan.value().digest().value);

  // Changing input order selects E before G, retaining repeated raw prefixes.
  std::swap(document.nodes[1].inputs[0], document.nodes[1].inputs[1]);
  ps::GraphContext e_first(document);
  auto alternative = compiler.compile(e_first, {}, {}, {{"optimize", {}}});
  PS_REQUIRE_OK(alternative);
  PS_CHECK(check_chains(alternative.value().ir_versions.front().ir.value(),
                        {{70, 60}, {70, 50}, {40, 30}, {70, 60, 10}, {20}}) ==
           0);
  PS_CHECK(check_chains(alternative.value().ir_versions.back().ir.value(),
                        {{70, 60}, {50}, {40, 30}, {10}, {20}}) == 0);
  auto other_result = execution.execute(alternative.value().plan.value());
  PS_REQUIRE_OK(other_result);
  PS_CHECK(ps::test::named_scalar(other_result.value(), "result") == 6.0);

  // Named roots are visited by name, retaining their shared ancestor paths.
  // The disconnected call stays in raw IR and is not demanded by execution.
  document.outputs = {{"z", 20, "value"}, {"a", 10, "value"}};
  document.nodes.push_back({80, "core.constant", {}, {{"value", 9.0}}});
  ps::GraphContext multiple(document);
  auto roots = compiler.compile(multiple, {}, {}, {{"optimize", {}}});
  PS_REQUIRE_OK(roots);
  PS_CHECK(
      check_chains(roots.value().ir_versions.front().ir.value(), {{70, 60, 10},
                                                                  {70, 60},
                                                                  {70, 50},
                                                                  {40, 30},
                                                                  {70, 60, 10},
                                                                  {20},
                                                                  {80}}) == 0);
  PS_CHECK(check_chains(roots.value().ir_versions.back().ir.value(),
                        {{70, 60, 10}, {50}, {40, 30}, {20}, {80}}) == 0);
  auto named = execution.execute(roots.value().plan.value());
  PS_REQUIRE_OK(named);
  PS_CHECK(ps::test::named_scalar(named.value(), "a") == 2.0);
  PS_CHECK(ps::test::named_scalar(named.value(), "z") == 6.0);
  std::reverse(document.outputs.begin(), document.outputs.end());
  ps::GraphContext reordered_roots(document);
  auto same_roots = compiler.analyze(reordered_roots.snapshot());
  PS_REQUIRE_OK(same_roots);
  PS_CHECK(same_roots.value().digest().value ==
           roots.value().ir_versions.front().ir.value().digest().value);

  // An input used twice is still one producer and a separate join continuation.
  ps::WorkflowDocument repeated;
  repeated.nodes = {
      {1, "core.constant", {}, {{"value", 3.0}}},
      {2,
       "math.add",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{1, "value"}},
       {}},
      {3, "core.identity", {WorkflowNodeOutput{2, "value"}}, {}}};
  repeated.outputs = {{"result", 3, "value"}};
  ps::GraphContext repeated_graph(repeated);
  auto shared = compiler.compile(repeated_graph, {}, {}, {{"optimize", {}}});
  PS_REQUIRE_OK(shared);
  PS_CHECK(check_chains(shared.value().ir_versions.front().ir.value(),
                        {{1}, {1}, {2, 3}}) == 0);
  PS_CHECK(check_chains(shared.value().ir_versions.back().ir.value(),
                        {{1}, {2, 3}}) == 0);
  auto doubled = execution.execute(shared.value().plan.value());
  PS_REQUIRE_OK(doubled);
  PS_CHECK(ps::test::named_scalar(doubled.value(), "result") == 6.0);

  repeated.nodes.resize(1);
  repeated.outputs = {{"a", 1, "value"}, {"b", 1, "value"}};
  ps::GraphContext duplicate_roots(repeated);
  auto aliases = compiler.compile(duplicate_roots, {}, {}, {{"optimize", {}}});
  PS_REQUIRE_OK(aliases);
  PS_CHECK(check_chains(aliases.value().ir_versions.front().ir.value(),
                        {{1}, {1}}) == 0);
  PS_CHECK(check_chains(aliases.value().ir_versions.back().ir.value(), {{1}}) ==
           0);
  auto alias_results = execution.execute(aliases.value().plan.value());
  PS_REQUIRE_OK(alias_results);
  PS_CHECK(ps::test::named_scalar(alias_results.value(), "a") == 3.0);
  PS_CHECK(ps::test::named_scalar(alias_results.value(), "b") == 3.0);

  repeated.inputs = {multi_result::declaration(1, "input")};
  repeated.nodes = {
      {1, "core.identity", {ps::WorkflowInputReference{1}}, {}},
      {2,
       "math.add",
       {WorkflowNodeOutput{1, "value"}, ps::WorkflowInputReference{1}},
       {}}};
  repeated.outputs = {{"result", 2, "value"}};
  ps::GraphContext external(repeated);
  auto bound_input = compiler.compile(external, {}, {}, {{"optimize", {}}});
  PS_REQUIRE_OK(bound_input);
  PS_CHECK(check_chains(bound_input.value().ir_versions.front().ir.value(),
                        {{1}, {2}}) == 0);
  auto external_result = execution.execute(
      bound_input.value().plan.value(),
      {{multi_result::binding(multi_result::take(execution.resource_budget()),
                              "input", 4.0)}});
  PS_REQUIRE_OK(external_result);
  PS_CHECK(ps::test::named_scalar(external_result.value(), "result") == 8.0);

  // A long unary path forms one block and uses the iterative traversal stack.
  ps::WorkflowDocument linear;
  linear.nodes = {{1, "core.constant", {}, {{"value", 1.0}}}};
  std::vector<ps::IRCallId> path{1};
  for (ps::IRCallId id = 2; id <= 512; ++id) {
    linear.nodes.push_back(
        {id, "core.identity", {WorkflowNodeOutput{id - 1, "value"}}, {}});
    path.push_back(id);
  }
  linear.outputs = {{"result", 512, "value"}};
  ps::GraphContext linear_graph(linear);
  auto line = compiler.analyze(linear_graph.snapshot());
  PS_REQUIRE_OK(line);
  PS_CHECK(check_chains(line.value(), {path}) == 0);
  // Full tree expansion is bounded even when a small DAG has exponential paths.
  linear.nodes.resize(1);
  for (ps::IRCallId id = 2; id <= 18; ++id)
    linear.nodes.push_back({id,
                            "math.add",
                            {WorkflowNodeOutput{id - 1, "value"},
                             WorkflowNodeOutput{id - 1, "value"}},
                            {}});
  linear.outputs = {{"result", 18, "value"}};
  ps::GraphContext expansive(linear);
  auto limited = compiler.analyze(expansive.snapshot());
  PS_CHECK(!limited.ok() &&
           limited.status().code == ps::ErrorCode::ResourceExhausted);
  return 0;
}
}  // namespace

/**
 * @brief Exercises typed stage identity, exact parameter bits, validation,
 * ordering, execution visibility, and staleness.
 * @return Zero when all checks pass.
 * @throws std::bad_alloc If test setup allocation fails.
 * @note Behavioral failures otherwise return nonzero through `PS_CHECK`.
 */
int main() {
  PS_CHECK(dependency_chains() == 0);
  ps::GraphSnapshot retained_revision;
  {
    ps::GraphContext revision_owner(ps::test::addition_document(1.0, 2.0));
    retained_revision = revision_owner.snapshot();
    PS_CHECK(retained_revision.current());
    revision_owner.replace(ps::test::addition_document(3.0, 4.0));
    PS_CHECK(!retained_revision.current());
    retained_revision = revision_owner.snapshot();
    PS_CHECK(retained_revision.current());
  }
  PS_CHECK(!retained_revision.current());

  using ps::Backend;
  using ps::Compiler;
  using ps::ErrorCode;
  using ps::GraphContext;
  using ps::GraphSnapshot;
  using ps::PlanningOptions;
  using ps::WorkflowDocument;
  using ps::WorkflowNode;
  using ps::WorkflowNodeOutput;
  using ps::WorkflowOutput;
  using ps::test::kernel_registry;

  auto operations = kernel_registry();
  Compiler compiler(operations);
  // Unknown operations reject consistently without retaining historical keys.
  const std::string unknown_key = "test.unknown_operation";
  const std::map<std::string, ps::ParameterValue> no_parameters;
  PS_CHECK(operations->find_traits(unknown_key).status().code ==
           ErrorCode::NotFound);
  ps::ResultProgramMetadata unknown_metadata;
  ps::ResultProgramQuery unknown_query(unknown_metadata, no_parameters);
  ps::ResourceBudget direct_root;
  PS_CHECK(
      operations
          ->start_result(unknown_key, unknown_query, direct_root.allocator())
          .status()
          .code == ErrorCode::NotFound);
  WorkflowDocument unknown_operation;
  unknown_operation.nodes = {{1, unknown_key, {}, {}}};
  unknown_operation.outputs = {{"result", 1, "value"}};
  GraphContext unknown_graph(std::move(unknown_operation));
  PS_CHECK(compiler.compile(unknown_graph).status().code ==
           ErrorCode::NotFound);
  GraphContext first(ps::test::addition_document(2.0, 3.0));
  auto compiled = compiler.compile(first);
  PS_CHECK(compiled.ok());
  PS_CHECK(compiled.value().ir_versions.front().ir.value().nodes().size() ==
           3U);
  PS_CHECK(
      compiled.value().ir_versions.front().ir.value().digest().value.size() ==
      16U);
  PS_CHECK(compiled.value().plan.value().digest().value.size() == 16U);
  PS_CHECK(compiled.value().plan.value().cache_key().value.size() == 16U);
  PS_CHECK(compiled.value().ir_versions.size() == 1);
  PS_CHECK(compiled.value().ir_versions.back().ir.value().digest().value !=
           compiled.value().plan.value().digest().value);
  PS_CHECK(compiled.value().plan.value().digest().value !=
           compiled.value().plan.value().cache_key().value);
  PS_CHECK(compiled.value().plan.value().steps().back().backend ==
           Backend::Cpu);

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
  PS_CHECK(compiled.value().ir_versions.front().ir.value().digest().value ==
           equivalent_compiled.value()
               .ir_versions.front()
               .ir.value()
               .digest()
               .value);

  auto signed_zero_operations = std::make_shared<ps::OperationRegistry>();
  ps::OperationDefinition signed_zero_definition;
  signed_zero_definition.key = "test.signed_zero";
  signed_zero_definition.traits.workspace_bytes = 8;
  signed_zero_definition.traits.parameter_schema = {ps::OperationParameterSpec{
      "value", ps::OperationParameterType::Float64, true}};
  signed_zero_definition.traits.outputs[0] = multi_result::output("value");
  signed_zero_definition.start_result =
      [](const ps::ResultProgramQuery& query,
         const ps::BufferAllocator& allocator) {
        const auto parameter = query.parameters.find("value");
        if (parameter == query.parameters.end())
          return ps::Result<ps::ResultContinuation>(
              ps::Status::failure(ErrorCode::InvalidArgument,
                                  "signed-zero probe parameter is missing"));
        const auto* value = std::get_if<double>(&parameter->second);
        if (!value)
          return ps::Result<ps::ResultContinuation>(ps::Status::failure(
              ErrorCode::InvalidArgument,
              "signed-zero probe parameter is not Float64"));
        return ps::ResultContinuation::make<multi_result::Program>(
            allocator, -1, std::signbit(*value) ? -1.0 : 1.0);
      };
  PS_CHECK(signed_zero_operations
               ->register_operation(std::move(signed_zero_definition))
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
  PS_CHECK(
      positive_zero.value().ir_versions.front().ir.value().digest().value !=
      negative_zero.value().ir_versions.front().ir.value().digest().value);
  PS_CHECK(positive_zero.value().plan.value().digest().value !=
           negative_zero.value().plan.value().digest().value);
  PS_CHECK(positive_zero.value().plan.value().cache_key().value !=
           negative_zero.value().plan.value().cache_key().value);
  ps::ExecutionContext signed_zero_execution(signed_zero_operations);
  auto positive_zero_result =
      signed_zero_execution.execute(positive_zero.value().plan.value());
  auto negative_zero_result =
      signed_zero_execution.execute(negative_zero.value().plan.value());
  if (!positive_zero_result.ok())
    std::cerr << positive_zero_result.status().message << " code="
              << static_cast<unsigned>(positive_zero_result.status().code)
              << " reason="
              << static_cast<unsigned>(positive_zero_result.status().reason)
              << "\n";
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
  std::get<WorkflowNodeOutput>(missing.nodes.back().inputs.back()).source_node =
      99U;
  GraphContext missing_context(std::move(missing));
  auto missing_result = compiler.compile(missing_context);
  PS_CHECK(!missing_result.ok());
  PS_CHECK(missing_result.status().code == ErrorCode::NotFound);

  WorkflowDocument cycle;
  cycle.nodes = {
      WorkflowNode{1U, "core.identity", {WorkflowNodeOutput{2U, "value"}}, {}},
      WorkflowNode{2U, "core.identity", {WorkflowNodeOutput{1U, "value"}}, {}},
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
  gpu_options.execution_mode = ps::ExecutionMode::NativeGpu;
  auto gpu_compiled = compiler.compile(first, gpu_options);
  PS_CHECK(gpu_compiled.ok());
  PS_CHECK(gpu_compiled.value().plan.value().steps().front().backend ==
           Backend::Cpu);
  PS_CHECK(gpu_compiled.value().plan.value().digest().value !=
           compiled.value().plan.value().digest().value);

  auto other_operations = kernel_registry();
  Compiler other_compiler(other_operations);
  auto mismatched_optimization =
      other_compiler.optimize(compiled.value().ir_versions.front().ir.value());
  PS_CHECK(!mismatched_optimization.ok());
  PS_CHECK(mismatched_optimization.status().code == ErrorCode::Stale);
  return 0;
}
