#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
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
  return 0;
}
