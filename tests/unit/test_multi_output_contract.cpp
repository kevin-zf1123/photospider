#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)

OperationDefinition definition() {
  OperationDefinition op;
  op.key = "test.results";
  op.traits.outputs[0].key = "first";
  op.traits.outputs.push_back(op.traits.outputs[0]);
  op.traits.outputs[1].key = "second";
  op.callback = [](const OperationInvocation& call) {
    return Result<Value>(Value::from_float64(10 + call.output_index));
  };
  return op;
}

int registration_and_invocation() {
  OperationRegistry registry;
  auto op = definition();
  PS_CHECK(registry.register_operation(op).ok());
  auto found = registry.find_traits(op.key);
  PS_CHECK(found.ok() && found.value().outputs.size() == 2);
  op.traits.outputs[1].key = "mutated";
  PS_CHECK(registry.find_traits(op.key).value().outputs[1].key == "second");
  std::vector<Value> inputs;
  std::vector<Region> demands;
  std::map<std::string, ParameterValue> parameters;
  OperationInvocation call(inputs, demands, parameters);
  call.output_index = 1;
  auto value = registry.invoke(op.key, call);
  PS_CHECK(value.ok() && value.value().as_float64().value() == 11);
  call.output_index = 2;
  PS_CHECK(!registry.invoke(op.key, call).ok());
  op = definition();
  op.key = "test.duplicate";
  op.traits.outputs[1].key = "first";
  PS_CHECK(!registry.register_operation(op).ok());
  op.traits.outputs.clear();
  PS_CHECK(!registry.register_operation(op).ok());
  op = definition();
  op.traits.cacheable = false;
  op.traits.side_effect_free = false;
  PS_CHECK(!registry.register_operation(op).ok());
  // A typed first result must not impose its schema on a generic sibling.
  auto mixed = definition();
  mixed.key = "test.mixed";
  auto& first = mixed.traits.outputs[0];
  first.output_schema.kind = OperationPortKind::Typed;
  first.output_schema.semantic_kind =
      static_cast<std::uint32_t>(SemanticKind::Scalar);
  first.output_semantic_rule = OperationSemanticRule::Establish;
  SemanticDescriptor scalar;
  scalar.channels = {{"value", "value", "dimensionless"}};
  first.output_facets = {encode_semantic(scalar).take_value()};
  PS_CHECK(registry.register_operation(mixed).ok());
  call.output_index = 1;
  PS_CHECK(registry.invoke(mixed.key, call).ok());
  OperationRegistry c_registry;
  PS_CHECK(c_registry.load_plugin(PS_MULTI_OUTPUT_FIXTURE).ok());
  call.output_index = 1;
  auto c_value = c_registry.invoke("test.c_results", call);
  PS_CHECK(c_value.ok() && c_value.value().as_float64().value() == 11);
  PS_CHECK(c_registry.find_traits("test.c_results").value().outputs[1].key ==
           "second");
  return 0;
}

struct SelectedState {
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    auto writer =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator);
    if (!writer.ok())
      return Result<DependencyPoll>(writer.status());
    const double number = 20 + phase.query.output_index;
    auto output = writer.take_value();
    std::memcpy(output.data(), &number, sizeof(number));
    auto value = std::move(output).publish();
    if (!value.ok())
      return Result<DependencyPoll>(value.status());
    auto fragments =
        ValueFragments::create(phase.query.output.descriptor, {},
                               phase.query.outputs, {value.take_value()});
    if (!fragments.ok())
      return Result<DependencyPoll>(fragments.status());
    return Result<DependencyPoll>(fragments.take_value());
  }
};
int staged_selection() {
  auto op = definition();
  op.key = "test.staged_results";
  op.callback = {};
  for (auto& output : op.traits.outputs) {
    output.region_rule = OperationRegionRule::Dependency;
    output.dependency_version = 1;
    output.continuation_bytes = sizeof(SelectedState);
    output.maximum_dependency_stages = 1;
  }
  op.traits.outputs[1].shape_rule = OperationShapeRule::Fixed;
  op.traits.outputs[1].fixed_output_shape = {2};
  op.start_dependency = [](const DependencyQuery&,
                           const BufferAllocator& allocator) {
    return DependencyContinuation::make<SelectedState>(allocator);
  };
  OperationRegistry registry;
  PS_CHECK(registry.register_operation(op).ok());
  std::vector<Value> inputs;
  std::vector<Region> demands;
  std::map<std::string, ParameterValue> parameters;
  OperationInvocation call(inputs, demands, parameters);
  call.output_index = 1;
  auto result = registry.invoke(op.key, call);
  PS_CHECK(result.ok());
  PS_CHECK(result.value().descriptor().shape == std::vector<std::uint64_t>{2});
  for (std::size_t i = 0; i < 2; ++i) {
    double value = 0;
    std::memcpy(&value, result.value().bytes().data() + i * sizeof(value),
                sizeof(value));
    PS_CHECK(value == 21);
  }
  return 0;
}

int compilation() {
  auto registry = std::make_shared<OperationRegistry>();
  auto op = definition();
  op.traits.outputs[0].shape_rule = OperationShapeRule::Fixed;
  op.traits.outputs[0].fixed_output_shape = {3, 5};
  op.traits.outputs[1].shape_rule = OperationShapeRule::Fixed;
  op.traits.outputs[1].fixed_output_shape = {2};
  op.traits.outputs[1].output_element_type = ElementType::Int64;
  PS_CHECK(registry->register_operation(op).ok());
  OperationDefinition identity;
  identity.key = "test.identity";
  identity.traits.input_count = 1;
  identity.traits.input_schema.resize(1);
  identity.traits.outputs[0].shape_rule =
      OperationShapeRule::PreserveFirstInput;
  identity.traits.outputs[0].output_dtype_rule = OperationDtypeRule::Input;
  identity.callback = [](const OperationInvocation& call) {
    return Result<Value>(call.inputs[0]);
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
  PS_CHECK(
      compiled.value().semantic.nodes()[0].outputs[1].descriptor.element_type ==
      ElementType::Int64);
  const auto& steps = compiled.value().plan.steps();
  PS_CHECK(steps.size() == 2);
  PS_CHECK((steps[0].result_ref() == ValueRef{1, 1}));
  PS_CHECK(steps[0].output_descriptor.shape == std::vector<std::uint64_t>{2});
  PS_CHECK(std::get<PlanStepInput>(steps[1].inputs[0]).step_index == 0);
  document.outputs.push_back({"first", 1, "first"});
  GraphContext both(document);
  auto all = compiler.compile(both);
  PS_CHECK(all.ok() && all.value().plan.steps().size() == 3);
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
  auto& crop = traits.outputs[0];
  crop.shape_rule = OperationShapeRule::Axes;
  crop.output_axes = {{OperationExtentSource::InputAxis, 1, {}, 0, 0, 0}};
  crop.output_axes[0].subtract_parameter = "split";
  auto& kernel = traits.outputs[1];
  kernel.shape_rule = OperationShapeRule::Axes;
  kernel.output_axes = {
      {OperationExtentSource::CeilParameter, 1, "radius", 0, 0, 1}};
  kernel.output_axes[0].multiplier = 2;
  kernel.input_indices = std::vector<std::uint32_t>{};
  OperationRegistry registry;
  PS_CHECK(registry.register_operation(op).ok());
  const std::vector<OperationMetadata> inputs = {
      {{ElementType::Float64, {7}}, {}}};
  for (double radius : {0., .25, 1., 1.25, 2., 64.}) {
    const std::map<std::string, ParameterValue> parameters = {
        {"radius", radius},
        {"split", std::int64_t{3}}};
    auto resolved = resolve_operation_traits(traits, 1, parameters);
    PS_CHECK(resolved.ok());
    auto outputs =
        infer_operation_outputs(resolved.value(), inputs, parameters);
    PS_CHECK(outputs.ok() && outputs.value().size() == 2);
    PS_CHECK(outputs.value()[0].descriptor.shape ==
             std::vector<std::uint64_t>{4});
    PS_CHECK(outputs.value()[1].descriptor.shape[0] ==
             2 * static_cast<std::uint64_t>(std::ceil(radius)) + 1);
  }
  const std::map<std::string, ParameterValue> bad = {
      {"radius", std::numeric_limits<double>::infinity()},
      {"split", std::int64_t{3}}};
  PS_CHECK(!resolve_operation_traits(traits, 1, bad).ok());
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
