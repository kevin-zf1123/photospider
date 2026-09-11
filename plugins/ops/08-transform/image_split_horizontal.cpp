#include <array>
#include <utility>
#include <vector>

#include "00-foundation/multi_output.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
struct State {
  bool requested = false;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    const auto atom = multi_output::coordinate(phase);
    const auto split = static_cast<std::uint64_t>(
        std::get<std::int64_t>(phase.query.parameters.at("split_x")));
    const auto source_x = atom[1] + (phase.query.output_index == 2 ? split : 0);
    const auto channels = phase.query.inputs[0].descriptor.shape[2];
    const Region source({{atom[0], 1}, {source_x, 1}, {0, channels}});
    if (!requested) {
      requested = true;
      auto footprint = Footprint::from_regions(
          phase.query.inputs[0].descriptor.shape, {source}, phase.sets);
      if (!footprint.ok())
        return Result<DependencyPoll>(footprint.status());
      return multi_output::need(phase, {{0, 5, footprint.take_value(), {}}});
    }
    for (const auto& fragment : phase.inputs[0].fragments()) {
      auto view = fragment.view(source);
      if (!view.ok())
        continue;
      auto address = fragment.byte_address({atom[0], source_x, 0});
      if (!address.ok())
        return Result<DependencyPoll>(address.status());
      auto output = Value::from_storage(
          phase.query.output.descriptor, phase.query.outputs.boxes()[0],
          {address.value(),
           fragment.layout().byte_strides,
           {atom[0], atom[1], 0}},
          fragment.storage(), phase.query.output.facets);
      if (!output.ok())
        return Result<DependencyPoll>(output.status());
      return multi_output::finish(phase, output.take_value());
    }
    return Result<DependencyPoll>(
        Status{ErrorCode::InvalidArgument, "crop input pixel is not supplied"});
  }
};
struct Joint {
  std::array<State, 3> states;
  Result<std::vector<DependencyAtomOutcome>> poll(
      const DependencyJointPhase& phase) {
    std::vector<DependencyAtomOutcome> outcomes;
    for (const auto* member : phase.members) {
      const auto id = member->query.output_index;
      outcomes.push_back({id, states[id].poll(*member)});
    }
    return Result<std::vector<DependencyAtomOutcome>>(std::move(outcomes));
  }
};
}  // namespace
Status register_image_split_horizontal(OperationRegistry* registry) {
  OperationDefinition op;
  op.key = "image.split_horizontal";
  auto& traits = op.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  auto& input = traits.input_schema[0];
  input.kind = OperationPortKind::Typed;
  input.semantic_kind = static_cast<std::uint32_t>(SemanticKind::Image);
  input.element_type = static_cast<std::uint32_t>(ElementType::Float32);
  input.rank = 3;
  traits.parameter_schema = {{"split_x", OperationParameterType::Int64, true}};
  traits.outputs.resize(3);
  constexpr const char* names[] = {"full", "left", "right"};
  for (std::uint32_t i = 0; i < 3; ++i) {
    auto& output = traits.outputs[i];
    output.key = names[i];
    output.input_indices = std::vector<std::uint32_t>{0};
    output.output_element_type = ElementType::Float32;
    output.shape_rule = OperationShapeRule::Axes;
    output.output_axes = {multi_output::input_axis(0, 0),
                          multi_output::input_axis(0, 1),
                          multi_output::input_axis(0, 2)};
    if (i == 1) {
      output.output_axes[1] = OperationExtent{};
      output.output_axes[1].source = OperationExtentSource::Parameter;
      output.output_axes[1].parameter = "split_x";
    } else if (i == 2) {
      output.output_axes[1].subtract_parameter = "split_x";
    }
    output.region_rule = OperationRegionRule::Dependency;
    output.dependency_version = 1;
    output.continuation_bytes = sizeof(State);
    output.maximum_dependency_stages = 2;
    output.failure_delivery = FailureDelivery::PerAtomOutcome;
    output.output_schema = input;
    output.output_semantic_rule = OperationSemanticRule::PreserveInput;
  }
  traits.joint_contract = 1;
  traits.joint_continuation_bytes = sizeof(Joint);
  op.validate_dependency = [](const auto& inputs, const auto& parameters) {
    const auto split = std::get<std::int64_t>(parameters.at("split_x"));
    if (split <= 0 ||
        static_cast<std::uint64_t>(split) >= inputs[0].descriptor.shape[1])
      return Status{ErrorCode::InvalidArgument,
                    "split_x must be strictly inside image width"};
    return Status::success();
  };
  op.start_dependency = [](const DependencyQuery&,
                           const BufferAllocator& allocator) {
    return DependencyContinuation::make<State>(allocator);
  };
  op.start_joint = [](const auto&, const BufferAllocator& allocator) {
    return DependencyJointContinuation::make<Joint>(allocator);
  };
  return registry->register_operation(std::move(op));
}
}  // namespace ps::plugin_internal
