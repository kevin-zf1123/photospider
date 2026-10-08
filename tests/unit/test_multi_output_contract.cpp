#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)

OperationDefinition definition() {
  OperationDefinition op;
  op.key = "test.results";
  op.traits.workspace_bytes = 2048;
  op.traits.outputs = {multi_result::output("first"),
                       multi_result::output("second")};
  op.start_result = [](const ResultProgramQuery& query,
                       const BufferAllocator& allocator) {
    return ResultContinuation::make<multi_result::Program>(
        allocator, -1, 10.0 + query.output_index);
  };
  return op;
}

Result<ResultRef> invoke_constant(const OperationRegistry& registry,
                                  const std::string& key, unsigned output) {
  auto traits = registry.find_traits(key);
  if (!traits.ok())
    return Result<ResultRef>(traits.status());
  ResultProgramMetadata metadata;
  metadata.output.result_schema = std::make_shared<const SchemaTemplate>(
      *traits.value()
           .outputs[output < traits.value().outputs.size() ? output : 0]
           .result_schema);
  const std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query(metadata, parameters);
  query.output_index = output;
  query.semantic_key = key;
  ResourceBudget root;
  auto allocator = root.allocator();
  auto state = registry.start_result(key, query, allocator);
  if (!state.ok())
    return Result<ResultRef>(state.status());

  ResultObjectInputs objects;
  ResourceVector<ResultIoReply> io;
  ResultProgramPhase phase{
      query,
      objects,
      io,
      allocator,
      root,
      [&](std::uint64_t units) { return root.consume({units}); },
      std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok)};
  auto continuation = state.take_value();
  auto polled = continuation.poll(phase);
  if (!polled.ok())
    return Result<ResultRef>(polled.status());
  const auto* publication = std::get_if<ResultPublication>(&polled.value());
  if (!publication || !publication->complete)
    return Result<ResultRef>(
        Status{ErrorCode::OperationFailed, "missing constant publication"});
  return Result<ResultRef>(publication->result);
}

int registration_and_invocation() {
  OperationRegistry registry;
  auto op = definition();
  PS_CHECK(registry.register_operation(op).ok());
  auto found = registry.find_traits(op.key);
  PS_CHECK(found.ok() && found.value().outputs.size() == 2);
  op.traits.outputs[1].key = "mutated";
  PS_CHECK(registry.find_traits(op.key).value().outputs[1].key == "second");
  auto value = invoke_constant(registry, op.key, 1);
  PS_CHECK(value.ok() && multi_result::number(value.value()) == 11);
  PS_CHECK(!invoke_constant(registry, op.key, 2).ok());
  op = definition();
  op.key = "test.duplicate";
  op.traits.outputs[1].key = "first";
  PS_CHECK(!registry.register_operation(op).ok());
  op.traits.outputs.clear();
  PS_CHECK(!registry.register_operation(op).ok());
  op = definition();
  op.key = "test.nonfree";
  op.traits.cacheable = false;
  op.traits.side_effect_free = false;
  PS_CHECK(!registry.register_operation(op).ok());
  // A typed first result must not impose its schema on a generic sibling.
  auto mixed = definition();
  mixed.key = "test.mixed";
  auto& first = mixed.traits.outputs[0];
  first.output_schema.semantic_kind =
      static_cast<std::uint32_t>(SemanticKind::Scalar);
  SemanticDescriptor scalar;
  scalar.channels = {{"value", "value", "dimensionless"}};
  first.result_schema->tensors[0].facets = {
      encode_semantic(scalar).take_value()};
  PS_CHECK(registry.register_operation(mixed).ok());
  auto generic = invoke_constant(registry, mixed.key, 1);
  PS_CHECK(generic.ok());
  PS_CHECK(generic.value().schema().tensors[0].facets.empty());
  PS_CHECK(multi_result::number(generic.value()) == 11);
  auto c_registry = std::make_shared<OperationRegistry>();
  PS_CHECK(c_registry->load_plugin(PS_MULTI_OUTPUT_FIXTURE).ok());
  PS_CHECK(c_registry->find_traits("test.c_results").value().outputs[1].key ==
           "second");
  PS_CHECK(c_registry->freeze().ok());
  WorkflowDocument c_document;
  c_document.nodes = {{1, "test.c_results", {}, {}}};
  c_document.outputs = {{"selected", 1, "second"}};
  GraphContext c_graph(c_document);
  auto c_plan = Compiler(c_registry).compile(c_graph);
  PS_CHECK(c_plan.ok());
  ExecutionContext c_execution(c_registry);
  auto c_value = c_execution.execute(c_plan.value().plan);
  PS_CHECK(c_value.ok() &&
           multi_result::number(c_value.value().results.at("selected")) == 11);
  return 0;
}

int staged_selection() {
  auto op = definition();
  op.key = "test.staged_results";
  op.traits.outputs[1] = multi_result::output(
      "second", multi_result::schema(ElementType::Float64, {2}));
  op.start_result = [](const ResultProgramQuery& query,
                       const BufferAllocator& allocator) {
    return ResultContinuation::make<multi_result::Program>(
        allocator, -1, 20.0 + query.output_index);
  };
  OperationRegistry registry;
  PS_CHECK(registry.register_operation(op).ok());
  auto result = invoke_constant(registry, op.key, 1);
  PS_CHECK(result.ok());
  PS_CHECK(result.value().schema().tensors[0].sample_shape() ==
           std::vector<std::uint64_t>{2});
  PS_CHECK(multi_result::number(result.value(), {0}) == 21);
  PS_CHECK(multi_result::number(result.value(), {1}) == 21);
  return 0;
}

int compilation() {
  auto registry = std::make_shared<OperationRegistry>();
  auto op = definition();
  op.traits.outputs[0] = multi_result::output(
      "first", multi_result::schema(ElementType::Float64, {3, 5}));
  op.traits.outputs[1] = multi_result::output(
      "second", multi_result::schema(ElementType::Int64, {2}));
  PS_CHECK(registry->register_operation(op).ok());
  OperationDefinition identity;
  identity.key = "test.identity";
  identity.traits.input_count = 1;
  identity.traits.input_schema.resize(1);
  identity.traits.input_schema[0].kind = OperationPortKind::Result;
  identity.traits.input_schema[0].tensor_key = "number";
  identity.traits.outputs[0] = multi_result::output("value");
  identity.traits.requires_metadata_specialization = true;
  identity.specialize_metadata = [](const auto& inputs, const auto&) {
    OperationOutputSpecialization output;
    output.metadata.result_schema = inputs[0].result_schema;
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  identity.start_result = [](const ResultProgramQuery&,
                             const BufferAllocator& allocator) {
    return ResultContinuation::make<multi_result::Program>(allocator, 0);
  };
  PS_CHECK(registry->register_operation(identity).ok());
  auto mixed_record = definition();
  mixed_record.key = "test.record_and_value";
  mixed_record.traits.outputs[0].observation_kind =
      ObservationKind::RequestRecord;
  PS_CHECK(registry->register_operation(mixed_record).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.nodes = {{1, op.key, {}, {}},
                    {2, identity.key, {WorkflowNodeOutput{1, "second"}}, {}}};
  document.outputs = {{"selected", 2, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  PS_CHECK(compiled.value().semantic.nodes()[0].outputs.size() == 2);
  PS_CHECK(compiled.value()
               .semantic.nodes()[0]
               .outputs[1]
               .result_schema->tensors[0]
               .descriptor.element_type == ElementType::Int64);
  const auto& steps = compiled.value().plan.steps();
  PS_CHECK(steps.size() == 2);
  PS_CHECK((steps[0].result_ref() == ValueRef{1, 1}));
  PS_CHECK(steps[0].output_result_schema->tensors[0].sample_shape() ==
           std::vector<std::uint64_t>{2});
  PS_CHECK(std::get<PlanStepInput>(steps[1].inputs[0]).step_index == 0);
  ExecutionContext context(registry);
  auto executed = context.execute(compiled.value().plan);
  PS_CHECK(executed.ok());
  const auto& integer_result = executed.value().results.at("selected");
  std::int64_t integer = 0;
  PS_CHECK(
      integer_result
          .read_tensor(integer_result.descriptor().value(), 0, {1}, &integer, 8)
          .ok());
  PS_CHECK(integer == 11);
  document.outputs.push_back({"first", 1, "first"});
  GraphContext both(document);
  auto all = compiler.compile(both);
  PS_CHECK(all.ok() && all.value().plan.steps().size() == 3);
  auto all_executed = context.execute(all.value().plan);
  PS_CHECK(all_executed.ok());
  PS_CHECK(multi_result::number(all_executed.value().results.at("first"),
                                {2, 4}) == 10);
  document.outputs.back().port = "absent";
  GraphContext bad(document);
  PS_CHECK(!compiler.compile(bad).ok());
  document.nodes[0].operation = mixed_record.key;
  document.outputs = {{"selected", 2, "value"}};
  GraphContext atomic_sibling(document);
  PS_CHECK(compiler.compile(atomic_sibling).ok());
  document.nodes[1].inputs = {WorkflowNodeOutput{1, "first"}};
  GraphContext terminal_consumer(document);
  PS_CHECK(!compiler.compile(terminal_consumer).ok());
  return 0;
}

int inference() {
  auto op = definition();
  auto& traits = op.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.parameter_schema = {
      {"radius", OperationParameterType::Float64, true, true, 0, 64},
      {"split", OperationParameterType::Int64, true, true, 1, 100}};
  traits.input_schema[0].kind = OperationPortKind::Result;
  traits.input_schema[0].tensor_key = "number";
  traits.outputs[1].input_indices = std::vector<std::uint32_t>{};
  traits.requires_metadata_specialization = true;
  op.specialize_metadata = [](const auto& inputs, const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    const auto extent = inputs[0].result_schema->tensors[0].sample_shape()[0];
    const auto split = static_cast<std::uint64_t>(
        std::get<std::int64_t>(parameters.at("split")));
    if (split >= extent)
      return Result<std::vector<OperationOutputSpecialization>>(
          Status{ErrorCode::InvalidArgument, "invalid split"});
    const auto radius = std::get<double>(parameters.at("radius"));
    std::vector<OperationOutputSpecialization> outputs(2);
    outputs[0].metadata.result_schema = std::make_shared<const SchemaTemplate>(
        multi_result::schema(ElementType::Float64, {extent - split}));
    outputs[1].metadata.result_schema =
        std::make_shared<const SchemaTemplate>(multi_result::schema(
            ElementType::Float64,
            {2 * static_cast<std::uint64_t>(std::ceil(radius)) + 1}));
    return Result<std::vector<OperationOutputSpecialization>>(
        std::move(outputs));
  };
  OperationRegistry registry;
  PS_CHECK(registry.register_operation(op).ok());
  std::vector<OperationMetadata> inputs(1);
  inputs[0].result_schema = std::make_shared<const SchemaTemplate>(
      multi_result::schema(ElementType::Float64, {7}));
  for (double radius : {0., .25, 1., 1.25, 2., 64.}) {
    const std::map<std::string, ParameterValue> parameters = {
        {"radius", radius},
        {"split", std::int64_t{3}}};
    auto resolved = registry.resolve_traits(op.key, inputs, parameters);
    PS_CHECK(resolved.ok());
    auto outputs =
        infer_operation_outputs(resolved.value(), inputs, parameters);
    PS_CHECK(outputs.ok() && outputs.value().size() == 2);
    PS_CHECK(outputs.value()[0].result_schema->tensors[0].sample_shape() ==
             std::vector<std::uint64_t>{4});
    PS_CHECK(outputs.value()[1].result_schema->tensors[0].sample_shape()[0] ==
             2 * static_cast<std::uint64_t>(std::ceil(radius)) + 1);
  }
  const std::map<std::string, ParameterValue> bad = {
      {"radius", std::numeric_limits<double>::infinity()},
      {"split", std::int64_t{3}}};
  PS_CHECK(!registry.resolve_traits(op.key, inputs, bad).ok());
  return 0;
}
}  // namespace

int main() {
  PS_CHECK(registration_and_invocation() == 0);
  PS_CHECK(inference() == 0);
  PS_CHECK(staged_selection() == 0);
  PS_CHECK(compilation() == 0);
  return 0;
}
