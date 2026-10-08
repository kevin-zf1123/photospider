#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/result_numeric_observation_fixture.hpp"

namespace {
using namespace ps::test_numeric;  // NOLINT(build/namespaces)
void expression_workflows() {
  Driver d;
  auto start = d.source({ElementType::Float32, {1}}, {0}, {1, {INT64_MIN}});
  auto end = d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  auto a = d.source({ElementType::Float64, {1}}, {double_bits(2)}, {1, {0}});
  auto b = d.source({ElementType::Float32, {1}}, {float_bits(1)}, {1, {0}});
  for (auto profile :
       {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon,
        CpuNumericProfile::X86Avx2}) {
    auto node = take(numeric::sample_expression_node(
        7, "a*x+b", WorkflowInputReference{11}, WorkflowInputReference{12}, 5,
        {{"a", WorkflowInputReference{13}}, {"b", WorkflowInputReference{14}}},
        ElementType::Float64, profile));
    std::vector<OperationMetadata> metadata;
    for (const auto& input : {start, end, a, b}) {
      OperationMetadata item;
      item.result_schema = std::make_shared<SchemaTemplate>(input.schema());
      metadata.push_back(std::move(item));
    }
    auto available =
        d.registry->resolve_traits(node.operation, metadata, node.parameters);
    if (!available.ok()) {
      require(profile != CpuNumericProfile::Strict &&
                  available.status().code == ErrorCode::BackendUnavailable,
              "unavailable expression profile reports BackendUnavailable");
      continue;
    }
    auto prepared =
        d.prepare(node.operation, {start, end, a, b}, node.parameters);
    auto document = prepared.graph->snapshot().document();
    document.outputs.push_back({"axis", 7, "axis"});
    auto graph = std::make_shared<GraphContext>(document);
    auto compiled = take(Compiler(d.registry).compile(*graph));
    require(compiled.plan.steps().size() == 2 &&
                compiled.plan.steps()[0].prepared->state() &&
                compiled.plan.steps()[0].prepared->state() ==
                    compiled.plan.steps()[1].prepared->state(),
            "named expression outputs share one immutable prepared AST");
    std::fenv_t saved;
    require(std::fegetenv(&saved) == 0 && std::fesetround(FE_DOWNWARD) == 0,
            "expression floating-environment fixture setup");
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_DIVBYZERO);
    auto result = take(d.context->execute_fragments(
        take(d.context->freeze(compiled.plan, prepared.bindings)),
        {{"out", take(Footprint::all({5}))},
         {"axis", take(Footprint::from_regions({3}, {Region({{1, 1}})}))}}));
    const bool restored = std::fegetround() == FE_DOWNWARD &&
                          std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO;
    std::fesetenv(&saved);
    require(restored, "expression restores caller floating environment");
    for (uint64_t i = 0; i < 5; ++i)
      require(
          read_bits(result.results.at("out"), {i}) == double_bits(1 + i * .5),
          "expression reads mixed-dtype named Result coefficients");
    const auto& axis = result.results.at("axis");
    require(axis.schema().tensors[0].atomic_trailing_axes == 1 &&
                take(axis.descriptor()).tensor_coverage(0) ==
                    take(Footprint::all({3})) &&
                read_bits(axis, {0}) == 0 &&
                read_bits(axis, {1}) == double_bits(1) &&
                read_bits(axis, {2}) == double_bits(.25),
            "axis projection retains a complete atomic Float64 sampling tuple");
    auto changed = take(result.dependencies.potential_dirty(
        "input2", take(Footprint::all({1})), 1));
    require(changed.at("out") == take(Footprint::all({5})) &&
                changed.at("axis").empty(),
            "coefficient edits invalidate values but never axis");
    prepared.bindings.inputs[2].result =
        d.source({ElementType::Float64, {1}}, {double_bits(3)}, {1, {0}});
    prepared.bindings.inputs[3].result =
        d.source({ElementType::Float32, {1}}, {float_bits(-1)}, {1, {0}});
    auto repeated = take(d.context->execute_fragments(
        take(d.context->freeze(compiled.plan, prepared.bindings)),
        {{"out", take(Footprint::all({5}))},
         {"axis", take(Footprint::all({3}))}}));
    for (uint64_t i = 0; i < 5; ++i)
      require(read_bits(repeated.results.at("out"), {i}) ==
                  double_bits(-1 + i * .75),
              "compiled expression reuses static preparation with new dynamic "
              "bindings");
    require(read_bits(repeated.results.at("axis"), {2}) == double_bits(.25),
            "coefficient rebinding preserves independently computed sampling "
            "metadata");
  }
  auto node = take(numeric::sample_expression_node(
      7, "x*x", WorkflowInputReference{11}, WorkflowInputReference{12}, 5, {},
      ElementType::Float32));
  auto descending =
      take(d.run(node.operation, {end, start}, {}, node.parameters))
          .results.at("out");
  for (uint64_t i = 0; i < 5; ++i) {
    const float x = 1 - i * .25F;
    require(read_bits(descending, {i}) == float_bits(x * x),
            "descending sampling preserves coordinates and Float32 output "
            "conversion");
  }
  d.context.reset();
  require(read_bits(descending, {2}) == float_bits(.25F),
          "expression Result storage survives execution context retirement");
}
void expression_many_coefficients() {
  Driver d;
  auto scalar =
      d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  std::vector<ResultRef> inputs{scalar, scalar};
  std::map<std::string, WorkflowInput> coefficients;
  std::vector<std::string> names;
  for (unsigned i = 0; i < 128; ++i) {
    auto name = std::string("c") + (i < 100 ? "0" : "") + (i < 10 ? "0" : "") +
                std::to_string(i);
    names.push_back(name);
    coefficients.emplace(name, WorkflowInputReference{13 + i});
    inputs.push_back(
        d.source({ElementType::Float64, {1}}, {double_bits(i + 1)}, {1, {0}}));
  }
  while (names.size() > 1) {
    std::vector<std::string> paired;
    for (size_t i = 0; i < names.size(); i += 2)
      paired.push_back("(" + names[i] + "+" + names[i + 1] + ")");
    names = std::move(paired);
  }
  auto node = take(numeric::sample_expression_node(
      7, names.front(), WorkflowInputReference{11}, WorkflowInputReference{12},
      1, coefficients));
  auto result = take(d.run(node.operation, inputs, {}, node.parameters));
  require(read_bits(result.results.at("out"), {0}) == double_bits(8256),
          "128 coefficients preserve original port numbers across bounded Need "
          "envelopes");
  auto changed = take(result.dependencies.potential_dirty(
      "input129", take(Footprint::all({1})), 1));
  require(
      changed.at("out") == take(Footprint::all({1})),
      "final coefficient beyond the second envelope retains source support");
}
void expression_boundaries() {
  Driver d;
  auto zero = d.source({ElementType::Float64, {1}}, {0}, {1, {0}});
  auto one = d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  auto node = take(numeric::sample_expression_node(
      7, "ln(x)", WorkflowInputReference{11}, WorkflowInputReference{12}, 3));
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto failed = d.run(node.operation, {zero, one},
                      take(Footprint::from_regions({3}, {Region({{2, 1}})})),
                      node.parameters);
  require(
      !failed.ok() && failed.status().code == ErrorCode::OperationFailed &&
          failed.status().reason == FailureReason::InvalidDomain &&
          failed.status().detail.scope == FailureScope::Run &&
          !failed.status().detail.atom &&
          failed.status().message.find("sample=0 x=0") != std::string::npos &&
          failed.status().message.find("span=") != std::string::npos &&
          d.root.statistics().live[ResourceKind::Payload] == payload,
      "Whole expression failure outside Q retains sample, coordinate and "
      "source span");
  auto axis =
      take(d.run(node.operation, {zero, one}, {}, node.parameters, {}, "axis"))
          .results.at("out");
  require(read_bits(axis, {2}) == double_bits(.5),
          "axis-only skips expression domain evaluation");
  auto close = d.source({ElementType::Float64, {1}},
                        {UINT64_C(0x3ff0000000000001)}, {1, {0}});
  node = take(numeric::sample_expression_node(
      7, "x", WorkflowInputReference{11}, WorkflowInputReference{12}, 3));
  failed = d.run(node.operation, {one, close}, {}, node.parameters);
  require(!failed.ok() &&
              failed.status().reason == FailureReason::ArithmeticOverflow &&
              failed.status().message.find("duplicate adjacent") !=
                  std::string::npos,
          "values reject duplicate RN64 coordinates");
  axis =
      take(d.run(node.operation, {one, close}, {}, node.parameters, {}, "axis"))
          .results.at("out");
  require(read_bits(axis, {2}) == double_bits(0x1p-53),
          "axis-only does not scan coordinate distinctness");
  auto empty = take(d.run(node.operation, {one, one},
                          take(Footprint::none({3})), node.parameters));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty expression skips invalid equal endpoints");
  node = take(numeric::sample_expression_node(
      7, "1/x", WorkflowInputReference{11}, WorkflowInputReference{12}, 3));
  failed = d.run(node.operation, {zero, one}, {}, node.parameters);
  require(!failed.ok() && failed.status().reason == FailureReason::DivideByZero,
          "expression division retains its distinct diagnostic reason");
  OperationMetadata good, legacy;
  good.result_schema = std::make_shared<SchemaTemplate>(zero.schema());
  legacy.descriptor = {ElementType::Float64, {1}};
  auto rejected = d.registry->resolve_traits(node.operation, {legacy, good},
                                             node.parameters);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "expression rejects incompatible input metadata before static parser "
          "access");
  auto multiple = zero.schema();
  auto extra = multiple.tensors[0];
  extra.key = "extra";
  multiple.tensors.push_back(extra);
  OperationMetadata ambiguous;
  ambiguous.result_schema = std::make_shared<SchemaTemplate>(multiple);
  rejected = d.registry->resolve_traits(node.operation, {ambiguous, good},
                                        node.parameters);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "expression requires an unambiguous single tensor input");
  auto bad_parameters = node.parameters;
  bad_parameters["coefficient_names"] = std::string("unused");
  rejected =
      d.registry->resolve_traits(node.operation, {good, good}, bad_parameters);
  require(!rejected.ok() &&
              rejected.status().code == ErrorCode::InvalidArgument &&
              rejected.status().detail.origin == FailureOrigin::Schema,
          "static coefficient-name mismatch remains a schema failure");
  auto wide = d.source({ElementType::Float64, {1}},
                       {UINT64_C(0x7fefffffffffffff)}, {1, {0}});
  node = take(numeric::sample_expression_node(
      7, "a", WorkflowInputReference{11}, WorkflowInputReference{12}, 1,
      {{"a", WorkflowInputReference{13}}}, ElementType::Float32));
  failed = d.run(node.operation, {zero, zero, wide}, {}, node.parameters);
  require(!failed.ok() &&
              failed.status().reason == FailureReason::ArithmeticOverflow &&
              failed.status().message.find("final Float32 overflow") !=
                  std::string::npos,
          "expression final narrowing overflow retains its diagnostic");
  axis = take(d.run(node.operation, {zero, zero, wide}, {}, node.parameters, {},
                    "axis"))
             .results.at("out");
  require(read_bits(axis, {0}) == 0 && read_bits(axis, {2}) == 0,
          "axis remains independent of values final-conversion failure");
  Driver limited(30000);
  auto begin = limited.source({ElementType::Float64, {1}}, {0}, {1, {0}});
  auto end =
      limited.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  node = take(numeric::sample_expression_node(
      7, "x*x", WorkflowInputReference{11}, WorkflowInputReference{12}, 65));
  const auto live = limited.root.statistics().live.values;
  failed = limited.run(node.operation, {begin, end}, {}, node.parameters);
  require(!failed.ok() && failed.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] ==
                  live[static_cast<size_t>(ResourceKind::Payload)],
          "expression work exhaustion releases unpublished dense payload");
  limited.context.reset();
  require(limited.root.statistics().live.values == live,
          "expression failure releases execution resources after context "
          "retirement");
  CancellationSource stop;
  stop.cancel();
  failed =
      d.run(node.operation, {zero, one}, {}, node.parameters, stop.token());
  require(!failed.ok() && failed.status().code == ErrorCode::Cancelled,
          "expression pre-cancellation preserves host error");
}
void expression_projection() {
  Driver d;
  auto registry = make_default_operation_registry(false);
  auto starts = std::make_shared<unsigned>(0);
  OperationDefinition producer;
  producer.key = "test.expression.fail";
  auto& output = producer.traits.outputs[0];
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.result_schema_id = "photospider.tensor";
  output.output_schema.result_schema_version = 1;
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::Float64, {1}};
  schema.tensors.push_back(member);
  output.result_schema = schema;
  output.continuation_bytes = sizeof(FailingProducer);
  output.maximum_dependency_stages = 1;
  producer.start_result = [starts](const auto&, const auto& allocator) {
    ++*starts;
    return ResultContinuation::make<FailingProducer>(allocator);
  };
  require(registry->register_operation(std::move(producer)).ok(),
          "expression source registration");
  require(registry->freeze().ok(), "expression registry freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.registry = registry;
  d.context = std::make_unique<ExecutionContext>(registry, config);
  d.root = take(d.context->resource_budget());
  auto start =
      d.source({ElementType::Float64, {1}}, {double_bits(3)}, {1, {0}});
  auto end = d.source({ElementType::Float64, {1}}, {double_bits(4)}, {1, {0}});
  auto coefficient =
      d.source({ElementType::Float64, {1}}, {double_bits(2)}, {1, {0}});
  auto node = take(numeric::sample_expression_node(
      7, "a*x", WorkflowInputReference{11}, WorkflowInputReference{12}, 5,
      {{"a", WorkflowInputReference{13}}}));
  auto prepared =
      d.prepare(node.operation, {start, end, coefficient}, node.parameters);
  auto document = prepared.graph->snapshot().document();
  document.nodes.insert(document.nodes.begin(),
                        {3, "test.expression.fail", {}, {}});
  document.nodes.back().inputs[2] = WorkflowNodeOutput{3, "value"};
  document.outputs.push_back({"axis", 7, "axis"});
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(registry).compile(*graph));
  auto frozen = take(d.context->freeze(compiled.plan, prepared.bindings));
  auto result = take(d.context->execute_fragments(
      frozen, {{"axis", take(Footprint::all({3}))}}));
  require(*starts == 0 &&
              read_bits(result.results.at("axis"), {2}) == double_bits(.25),
          "axis-only never starts a failing coefficient producer");
  auto failed = d.context->execute_fragments(
      frozen, {{"out", take(Footprint::all({5}))}});
  require(!failed.ok() && failed.status().message == "must stay lazy" &&
              failed.status().detail.node_id == 3 && *starts == 1,
          "values propagate the required coefficient producer failure");
  *starts = 0;
  document.nodes.back().inputs[1] = WorkflowNodeOutput{3, "value"};
  document.nodes.back().inputs[2] = WorkflowInputReference{13};
  document.nodes.back().parameters["count"] = int64_t{1};
  graph = std::make_shared<GraphContext>(document);
  compiled = take(Compiler(registry).compile(*graph));
  result = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::all({1}))},
       {"axis", take(Footprint::all({3}))}}));
  require(*starts == 0 &&
              read_bits(result.results.at("out"), {0}) == double_bits(6) &&
              read_bits(result.results.at("axis"), {0}) == double_bits(3) &&
              read_bits(result.results.at("axis"), {1}) == double_bits(3) &&
              read_bits(result.results.at("axis"), {2}) == 0,
          "singleton values and axis exclude the failing end producer");
  auto association = result.results.at("axis").association();
  require(association == ResourceVector<uint64_t>{start.object_id()},
          "singleton axis association contains only active start owner");
  document.nodes.back().parameters["count"] = int64_t{5};
  document.nodes.back().inputs[2] = WorkflowNodeOutput{3, "value"};
  graph = std::make_shared<GraphContext>(document);
  compiled = take(Compiler(registry).compile(*graph));
  result = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::none({5}))},
       {"axis", take(Footprint::none({3}))}}));
  require(*starts == 0 &&
              take(result.results.at("out").descriptor())
                  .tensor_coverage(0)
                  .empty() &&
              take(result.results.at("axis").descriptor())
                  .tensor_coverage(0)
                  .empty(),
          "Empty expression outputs leave all failing producers unstarted");
}
}  // namespace
int main() {
  try {
    expression_workflows();
    expression_many_coefficients();
    expression_boundaries();
    expression_projection();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
