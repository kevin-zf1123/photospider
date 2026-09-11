#include <array>
#include <utility>
#include <vector>

#include "05-filter/regional_convolution.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
struct Joint {
  std::array<convolution::State, 3> states{{{true, 0}, {true, 1}, {true, 2}}};
  explicit Joint(const std::vector<DependencyQuery>& queries) {
    for (const auto& query : queries)
      states[query.output_index].channel = convolution::channel(query);
  }
  Result<std::vector<DependencyAtomOutcome>> poll(
      const DependencyJointPhase& phase) {
    std::vector<DependencyAtomOutcome> results;
    for (const auto* member : phase.members)
      results.push_back({member->query.output_index,
                         states[member->query.output_index].poll(*member)});
    return Result<std::vector<DependencyAtomOutcome>>(std::move(results));
  }
};
}  // namespace
Status register_image_convolve_channels(OperationRegistry* registry) {
  OperationDefinition op;
  op.key = "image.convolve_channels";
  auto& traits = op.traits;
  traits.input_count = 4;
  traits.input_schema.resize(4);
  for (auto& input : traits.input_schema) {
    input.element_type = static_cast<std::uint32_t>(ElementType::Float32);
    input.rank = 2;
  }
  auto& image = traits.input_schema[0];
  image.rank = 3;
  image.kind = OperationPortKind::Typed;
  image.semantic_kind = static_cast<std::uint32_t>(SemanticKind::Image);
  traits.outputs.resize(3);
  const char* names[] = {"r", "g", "b"};
  const char* roles[] = {"red", "green", "blue"};
  for (std::uint32_t i = 0; i < 3; ++i) {
    auto& output = traits.outputs[i];
    output.key = names[i];
    output.input_indices = std::vector<std::uint32_t>{0, i + 1};
    output.output_element_type = ElementType::Float32;
    output.shape_rule = OperationShapeRule::Axes;
    output.output_axes = {multi_output::input_axis(0, 0),
                          multi_output::input_axis(0, 1)};
    output.region_rule = OperationRegionRule::Dependency;
    output.dependency_version = 1;
    output.continuation_bytes = sizeof(convolution::State);
    output.maximum_dependency_stages = 2;
    output.failure_delivery = FailureDelivery::PerAtomOutcome;
    output.output_schema.kind = OperationPortKind::Typed;
    output.output_schema.semantic_kind =
        static_cast<std::uint32_t>(SemanticKind::ScalarField);
    output.output_semantic_rule = OperationSemanticRule::Establish;
    SemanticDescriptor semantic;
    semantic.kind = SemanticKind::ScalarField;
    semantic.unit = "relative";
    semantic.channels = {{names[i], roles[i], "relative"}};
    output.output_facets = {encode_semantic(semantic).take_value()};
  }
  traits.parameter_schema = convolution::parameters(true);
  traits.joint_contract = 1;
  traits.joint_continuation_bytes = sizeof(Joint);
  op.validate_dependency = [](const auto& inputs, const auto& parameters) {
    return convolution::validate(inputs, parameters, true);
  };
  op.start_dependency = [](const DependencyQuery& query,
                           const BufferAllocator& allocator) {
    return DependencyContinuation::make<convolution::State>(
        allocator, true, convolution::channel(query));
  };
  op.start_joint = [](const auto& queries, const BufferAllocator& allocator) {
    return DependencyJointContinuation::make<Joint>(allocator, queries);
  };
  return registry->register_operation(std::move(op));
}
}  // namespace ps::plugin_internal
