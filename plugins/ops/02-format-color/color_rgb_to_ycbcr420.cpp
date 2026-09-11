#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/multi_output.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
std::array<std::uint32_t, 3> channels(const DependencyQuery& query) {
  for (const auto& facet : query.inputs[0].facets) {
    if (facet.key != "photospider.image")
      continue;
    auto semantic = decode_semantic(facet).take_value();
    std::array<std::uint32_t, 3> result{};
    for (std::uint32_t i = 0; i < 3; ++i) {
      const auto& role = semantic.channels[i].role;
      result[role == "red" ? 0 : role == "green" ? 1 : 2] = i;
    }
    return result;
  }
  return {};
}
struct Converted {
  std::uint64_t y = 0, x = 0;
  std::array<double, 3> value{};
};
struct SharedPixels {
  std::array<Converted, 9> pixels;
  std::size_t count = 0;
};
struct State {
  std::array<std::uint32_t, 3> channels;
  bool requested = false;
  explicit State(std::array<std::uint32_t, 3> channels) : channels(channels) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase,
                              SharedPixels* shared) {
    const auto atom = multi_output::coordinate(phase);
    const bool chroma = phase.query.output_index != 0;
    const auto factor = chroma ? 2U : 1U;
    const auto y = atom[0] * factor, x = atom[1] * factor;
    const auto& shape = phase.query.inputs[0].descriptor.shape;
    const auto height = std::min<std::uint64_t>(factor, shape[0] - y);
    const auto width = std::min<std::uint64_t>(factor, shape[1] - x);
    if (!requested) {
      requested = true;
      auto pixels = Footprint::from_regions(
          shape, {Region({{y, height}, {x, width}, {0, 3}})}, phase.sets);
      if (!pixels.ok())
        return Result<DependencyPoll>(pixels.status());
      return multi_output::need(phase, {{0, 5, pixels.take_value(), {}}});
    }
    double sum = 0;
    for (std::uint64_t row = y; row < y + height; ++row) {
      for (std::uint64_t column = x; column < x + width; ++column) {
        std::array<double, 3> rgb{};
        // Every member performs its own authorized reads and domain checks.
        // The shared memo only removes repeated transfer/matrix arithmetic.
        for (unsigned c = 0; c < 3; ++c) {
          float sample;
          auto status = phase.read(0, {row, column, channels[c]}, &sample,
                                   sizeof(sample));
          if (!status.ok())
            return Result<DependencyPoll>(status);
          if (!std::isfinite(sample) || sample < 0 || sample > 1)
            return Result<DependencyPoll>(
                Status{ErrorCode::OperationFailed,
                       "420 input must be finite linear RGB in [0,1]"});
          rgb[c] = sample;
        }
        const Converted* found = nullptr;
        if (shared) {
          for (std::size_t i = 0; i < shared->count; ++i)
            if (shared->pixels[i].y == row && shared->pixels[i].x == column) {
              found = &shared->pixels[i];
              break;
            }
        }
        Converted computed{row, column, {}};
        if (!found) {
          for (auto& sample : rgb)
            sample = sample < .018 ? 4.5 * sample
                                   : 1.099 * std::pow(sample, .45) - .099;
          const auto luma = .2126 * rgb[0] + .7152 * rgb[1] + .0722 * rgb[2];
          computed.value = {luma, (rgb[2] - luma) / 1.8556,
                            (rgb[0] - luma) / 1.5748};
          found = &computed;
          if (shared && shared->count < shared->pixels.size()) {
            shared->pixels[shared->count] = computed;
            found = &shared->pixels[shared->count++];
          }
        }
        sum += found->value[phase.query.output_index];
      }
    }
    const auto value =
        static_cast<float>(sum / static_cast<double>(height * width));
    return multi_output::finish(phase, &value, sizeof(value));
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    return poll(phase, nullptr);
  }
};
struct Joint {
  std::array<State, 3> states;
  SharedPixels shared;
  explicit Joint(std::array<std::uint32_t, 3> channels)
      : states{State(channels), State(channels), State(channels)} {}
  Result<std::vector<DependencyAtomOutcome>> poll(
      const DependencyJointPhase& phase) {
    shared.count = 0;
    std::vector<DependencyAtomOutcome> result;
    for (const auto* member : phase.members) {
      const auto id = member->query.output_index;
      result.push_back({id, states[id].poll(*member, &shared)});
    }
    return Result<std::vector<DependencyAtomOutcome>>(std::move(result));
  }
};
}  // namespace
Status register_color_rgb_to_ycbcr420(OperationRegistry* registry) {
  OperationDefinition op;
  op.key = "color.rgb_to_ycbcr420";
  auto& traits = op.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  auto& input = traits.input_schema[0];
  input.kind = OperationPortKind::Typed;
  input.semantic_kind = static_cast<std::uint32_t>(SemanticKind::Image);
  input.element_type = static_cast<std::uint32_t>(ElementType::Float32);
  input.rank = 3;
  traits.outputs.resize(3);
  constexpr const char* names[] = {"y", "cb", "cr"};
  constexpr const char* roles[] = {"luma", "blue_difference", "red_difference"};
  for (std::uint32_t i = 0; i < 3; ++i) {
    auto& output = traits.outputs[i];
    output.key = names[i];
    output.input_indices = std::vector<std::uint32_t>{0};
    output.output_element_type = ElementType::Float32;
    output.shape_rule = OperationShapeRule::Axes;
    output.output_axes = {multi_output::input_axis(0, 0, i ? 2 : 1),
                          multi_output::input_axis(0, 1, i ? 2 : 1)};
    output.region_rule = OperationRegionRule::Dependency;
    output.dependency_version = 1;
    output.continuation_bytes = sizeof(State);
    output.maximum_dependency_stages = 2;
    output.failure_delivery = FailureDelivery::PerAtomOutcome;
    output.output_schema.kind = OperationPortKind::Typed;
    output.output_schema.semantic_kind =
        static_cast<std::uint32_t>(SemanticKind::ImagePlane);
    output.output_semantic_rule = OperationSemanticRule::YCbCrPlane;
    auto semantic = rgba_semantics();
    semantic.kind = SemanticKind::ImagePlane;
    semantic.channels = {{names[i], roles[i], "relative"}};
    semantic.model = "ycbcr";
    semantic.transfer = "bt709";
    semantic.association = "none";
    semantic.plane_origin =
        i ? std::array<double, 2>{.5, .5} : std::array<double, 2>{0, 0};
    semantic.plane_step =
        i ? std::array<double, 2>{2, 2} : std::array<double, 2>{1, 1};
    output.output_facets = {encode_semantic(semantic).take_value()};
  }
  traits.joint_contract = 1;
  traits.joint_continuation_bytes = sizeof(Joint);
  op.start_dependency = [](const DependencyQuery& query,
                           const BufferAllocator& allocator) {
    return DependencyContinuation::make<State>(allocator, channels(query));
  };
  op.start_joint = [](const std::vector<DependencyQuery>& queries,
                      const BufferAllocator& allocator) {
    return DependencyJointContinuation::make<Joint>(allocator,
                                                    channels(queries[0]));
  };
  return registry->register_operation(std::move(op));
}
}  // namespace ps::plugin_internal
