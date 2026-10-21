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
  op.traits.outputs = {multi_result::output("first"),
                       multi_result::output("second")};
  multi_result::install_fixed(op, {-1, -1}, {10, 11});
  return op;
}

Result<ResultRef> invoke_constant(
    const std::shared_ptr<OperationRegistry>& registry, const std::string& key,
    const std::string& output) {
  WorkflowDocument document;
  document.nodes = {{1, key, {}, {}}};
  document.outputs = {{"selected", 1, output}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  if (!compiled.ok())
    return Result<ResultRef>(compiled.status());
  ExecutionContext context(registry);
  auto result = multi_result::run(context, compiled.value().plan.value());
  return result.ok() ? Result<ResultRef>(result.value().results.at("selected"))
                     : Result<ResultRef>(result.status());
}

int registration_and_invocation() {
  auto registry = std::make_shared<OperationRegistry>();
  auto op = definition();
  PS_REQUIRE_OK(registry->register_operation(op));
  auto found = registry->find_traits(op.key);
  PS_CHECK(found.ok() && found.value().outputs.size() == 2);
  op.traits.outputs[1].key = "mutated";
  op.traits.outputs[1].result_schema->tensors[0].descriptor.shape = {99};
  const auto retained = registry->find_traits(op.key);
  PS_CHECK(
      retained.value().outputs[1].key == "second" &&
      retained.value().outputs[1].result_schema->tensors[0].sample_shape() ==
          std::vector<std::uint64_t>{1});
  op = definition();
  op.key = "test.duplicate";
  op.traits.outputs[1].key = "first";
  PS_CHECK(!registry->register_operation(op).ok());
  op.traits.outputs.clear();
  PS_CHECK(!registry->register_operation(op).ok());
  // Independent sibling schemas retain their own structural grouping.
  auto mixed = definition();
  mixed.key = "test.mixed";
  mixed.traits.outputs[0].result_schema->tensors[0].atomic_trailing_axes = 1;
  PS_REQUIRE_OK(registry->register_operation(mixed));
  PS_REQUIRE_OK(registry->freeze());
  auto value = invoke_constant(registry, "test.results", "second");
  PS_REQUIRE_OK(value);
  PS_CHECK(multi_result::number(value.value()) == 11);
  PS_CHECK(!invoke_constant(registry, "test.results", "absent").ok());
  auto generic = invoke_constant(registry, mixed.key, "second");
  PS_REQUIRE_OK(generic);
  PS_CHECK(generic.value().schema().tensors[0].atomic_trailing_axes == 0);
  PS_CHECK(multi_result::number(generic.value()) == 11);
  auto c_registry = std::make_shared<OperationRegistry>();
  PS_REQUIRE_OK(c_registry->load_plugin(PS_MULTI_OUTPUT_FIXTURE));
  PS_CHECK(c_registry->find_traits("test.c_results").value().outputs[1].key ==
           "second");
  PS_REQUIRE_OK(c_registry->freeze());
  auto c_value = invoke_constant(c_registry, "test.c_results", "second");
  PS_REQUIRE_OK(c_value);
  PS_CHECK(multi_result::number(c_value.value()) == 11);
  return 0;
}

int independent_shapes() {
  auto op = definition();
  op.key = "test.shaped_results";
  op.traits.outputs[1] = multi_result::output(
      "second", multi_result::schema(ElementType::Float64, {2}));
  multi_result::install_fixed(op, {-1, -1}, {20, 21});
  auto registry = std::make_shared<OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(op));
  PS_REQUIRE_OK(registry->freeze());
  auto result = invoke_constant(registry, op.key, "second");
  PS_REQUIRE_OK(result);
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

  identity.traits.input_schema[0].tensor_key = "number";
  identity.traits.outputs[0] = multi_result::output("value");
  identity.traits.requires_metadata_specialization = true;
  identity.specialize_metadata = [](const auto& inputs, const auto&) {
    OperationOutputSpecialization output;
    output.metadata.result_schema = inputs[0].result_schema;
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  multi_result::install_fixed(identity, {0}, {0});
  PS_CHECK(registry->register_operation(identity).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.nodes = {{1, op.key, {}, {}},
                    {2, identity.key, {WorkflowNodeOutput{1, "second"}}, {}}};
  document.outputs = {{"selected", 2, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  PS_CHECK(compiled.value()
               .ir_versions.front()
               .ir.value()
               .nodes()[0]
               .outputs.size() == 2);
  PS_CHECK(compiled.value()
               .ir_versions.front()
               .ir.value()
               .nodes()[0]
               .outputs[1]
               .result_schema->tensors[0]
               .descriptor.element_type == ElementType::Int64);
  const auto& plan = compiled.value().plan.value();
  const auto& program = plan.program();
  PS_CHECK(program.calls.size() == 2 && program.tasks.size() == 2);
  const auto first = plan.calls()[0].outputs[0];
  const auto second = plan.calls()[0].outputs[1];
  PS_CHECK(program.values[first].tiles.empty());
  PS_CHECK(program.values[second].tiles.size() == 1);
  PS_CHECK(plan.calls()[1].inputs == std::vector<physical::Id>{second});
  PS_CHECK(program.schemas[program.values[second].schema]
               ->tensors[0]
               .sample_shape() == std::vector<std::uint64_t>{2});
  const auto& identity_task = program.tasks[program.calls[1].tasks[0]];
  PS_CHECK(identity_task.reads.size() == 1 &&
           program.tiles[identity_task.reads[0].tile].value == second);
  ExecutionContext context(registry);
  auto executed = multi_result::run(context, plan);
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
  PS_REQUIRE_OK(all);
  PS_CHECK(all.value().plan.value().program().tasks.size() == 3);
  auto all_executed = multi_result::run(context, all.value().plan.value());
  PS_CHECK(all_executed.ok());
  PS_CHECK(multi_result::number(all_executed.value().results.at("first"),
                                {2, 4}) == 10);
  document.outputs.back().port = "absent";
  GraphContext bad(document);
  PS_CHECK(!compiler.compile(bad).ok());
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
    auto resolved = registry.prepare_operation(op.key, inputs, parameters);
    PS_CHECK(resolved.ok());
    auto outputs =
        infer_operation_outputs(resolved.value()->traits(), inputs, parameters);
    PS_CHECK(outputs.ok() && outputs.value().size() == 2);
    PS_CHECK(outputs.value()[0].result_schema->tensors[0].sample_shape() ==
             std::vector<std::uint64_t>{4});
    PS_CHECK(outputs.value()[1].result_schema->tensors[0].sample_shape()[0] ==
             2 * static_cast<std::uint64_t>(std::ceil(radius)) + 1);
  }
  const std::map<std::string, ParameterValue> bad = {
      {"radius", std::numeric_limits<double>::infinity()},
      {"split", std::int64_t{3}}};
  PS_CHECK(!registry.prepare_operation(op.key, inputs, bad).ok());
  return 0;
}
}  // namespace

int main() {
  PS_CHECK(registration_and_invocation() == 0);
  PS_CHECK(inference() == 0);
  PS_CHECK(independent_shapes() == 0);
  PS_CHECK(compilation() == 0);
  return 0;
}
