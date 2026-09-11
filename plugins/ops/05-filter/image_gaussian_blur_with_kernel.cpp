#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/basic_common.hpp"
#include "00-foundation/multi_output.hpp"
#include "data/input_validation.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
constexpr std::uint64_t maximum_kernel_bytes = 129 * 129 * sizeof(float);
struct Parameters {
  double radius, sigma;
  int extent, side;
  bool clamp;
  explicit Parameters(const std::map<std::string, ParameterValue>& parameters)
      : radius(std::get<double>(parameters.at("radius"))),
        sigma(std::get<double>(parameters.at("sigma"))),
        extent(static_cast<int>(std::ceil(radius))),
        side(2 * extent + 1),
        clamp(!parameters.count("boundary") ||
              std::get<std::string>(parameters.at("boundary")) == "clamp") {}
  double weight(int y, int x) const {
    if (sigma == 0)
      return x == 0 && y == 0 ? 1 : 0;
    const auto axis = [&](int d) {
      // Subtract the integer first: radius+1 can round away the positive
      // boundary weight immediately above an integer radius.
      return d == 0 ? 1. : std::clamp(radius - (std::abs(d) - 1), 0., 1.);
    };
    const double sx = x / sigma, sy = y / sigma;
    return std::exp(-.5 * (sx * sx + sy * sy)) * axis(x) * axis(y);
  }
};
Result<Value> make_kernel(const Parameters& parameters,
                          const BufferAllocator& allocator,
                          const std::function<Status(std::uint64_t)>& consume) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Result<Value>(
        Status{ErrorCode::OperationFailed, "kernel floating environment"});
  const auto side = static_cast<std::uint64_t>(parameters.side);
  auto status = consume(2 * side * side);
  if (!status.ok())
    return Result<Value>(status);
  double total = 0;
  for (int y = -parameters.extent; y <= parameters.extent; ++y) {
    status = consume(0);
    if (!status.ok())
      return Result<Value>(status);
    for (int x = -parameters.extent; x <= parameters.extent; ++x)
      total += parameters.weight(y, x);
  }
  const auto bytes = side * side * sizeof(float);
  auto allocated = MutableValue::allocate({ElementType::UInt8, {bytes}},
                                          Region::whole({bytes}), allocator);
  if (!allocated.ok())
    return Result<Value>(allocated.status());
  auto output = allocated.take_value();
  std::size_t index = 0;
  for (int y = -parameters.extent; y <= parameters.extent; ++y) {
    status = consume(0);
    if (!status.ok())
      return Result<Value>(status);
    for (int x = -parameters.extent; x <= parameters.extent; ++x) {
      const float coefficient =
          static_cast<float>(parameters.weight(y, x) / total);
      std::memcpy(output.data() + index++ * sizeof(float), &coefficient,
                  sizeof(coefficient));
    }
  }
  return std::move(output).publish();
}
struct State {
  Value kernel;
  bool requested = false;
  Result<DependencyPoll> poll_with_kernel(const DependencyPhase& phase) {
    const Parameters parameters(phase.query.parameters);
    const auto atom = multi_output::coordinate(phase);
    const auto side = static_cast<std::uint64_t>(parameters.side);
    if (phase.query.output_index == 1) {
      auto view = Value::from_storage(
          phase.query.output.descriptor, phase.query.outputs.boxes()[0],
          {(atom[0] * side + atom[1]) * sizeof(float),
           {static_cast<std::int64_t>(side * sizeof(float)), sizeof(float)},
           atom},
          kernel.storage());
      if (!view.ok())
        return Result<DependencyPoll>(view.status());
      return multi_output::finish(phase, view.take_value());
    }
    const auto& shape = phase.query.inputs[0].descriptor.shape;
    const auto channels = shape[2];
    if (!requested) {
      requested = true;
      std::uint64_t top, bottom, left, right;
      basic_internal::shifted(atom[0], -parameters.extent, shape[0], true,
                              &top);
      basic_internal::shifted(atom[0], parameters.extent, shape[0], true,
                              &bottom);
      basic_internal::shifted(atom[1], -parameters.extent, shape[1], true,
                              &left);
      basic_internal::shifted(atom[1], parameters.extent, shape[1], true,
                              &right);
      auto input = Footprint::from_regions(shape,
                                           {Region({{top, bottom - top + 1},
                                                    {left, right - left + 1},
                                                    {0, channels}})},
                                           phase.sets);
      if (!input.ok())
        return Result<DependencyPoll>(input.status());
      return multi_output::need(phase, {{0, 5, input.take_value(), {}}});
    }
    auto charged = phase.consume_work(side * side);
    if (!charged.ok())
      return Result<DependencyPoll>(charged);
    std::array<double, 4> sum{};
    for (std::uint64_t y = 0; y < side; ++y)
      for (std::uint64_t x = 0; x < side; ++x) {
        float coefficient;
        std::memcpy(&coefficient,
                    kernel.bytes().data() + (y * side + x) * sizeof(float),
                    sizeof(coefficient));
        std::uint64_t sy, sx;
        const bool inside =
            basic_internal::shifted(
                atom[0], parameters.extent - static_cast<std::int64_t>(y),
                shape[0], parameters.clamp, &sy) &&
            basic_internal::shifted(
                atom[1], parameters.extent - static_cast<std::int64_t>(x),
                shape[1], parameters.clamp, &sx);
        for (std::uint64_t c = 0; c < channels; ++c) {
          float sample = 0;
          if (inside) {
            auto status = phase.read(0, {sy, sx, c}, &sample, sizeof(sample));
            if (!status.ok())
              return Result<DependencyPoll>(status);
          }
          const double product = static_cast<double>(sample) * coefficient;
          sum[c] = sum[c] + product;
          if (!std::isfinite(sum[c]))
            return Result<DependencyPoll>(Status{
                ErrorCode::OperationFailed, "Gaussian accumulation overflow"});
        }
      }
    std::array<float, 4> samples{};
    for (std::uint64_t c = 0; c < channels; ++c) {
      if (std::abs(sum[c]) > std::numeric_limits<float>::max())
        return Result<DependencyPoll>(
            Status{ErrorCode::OperationFailed, "Gaussian Float32 overflow"});
      samples[c] = static_cast<float>(sum[c]);
    }
    return multi_output::finish(phase, samples.data(),
                                channels * sizeof(float));
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    const Parameters parameters(phase.query.parameters);
    if (!kernel.valid()) {
      auto cached = phase.checkpoint_before(1, 0);
      if (!cached.ok())
        return Result<DependencyPoll>(cached.status());
      if (cached.value()) {
        kernel = cached.value()->state();
      } else {
        auto created =
            make_kernel(parameters, phase.allocator, phase.consume_work);
        if (!created.ok())
          return Result<DependencyPoll>(created.status());
        kernel = created.take_value();
        auto status = phase.checkpoint_publish(1, 0, kernel);
        if (!status.ok())
          return Result<DependencyPoll>(status);
      }
    }
    return poll_with_kernel(phase);
  }
};
struct Joint {
  Value kernel;
  std::array<State, 2> states;
  Result<std::vector<DependencyAtomOutcome>> poll(
      const DependencyJointPhase& phase) {
    if (!kernel.valid()) {
      auto created = make_kernel(Parameters(phase.members[0]->query.parameters),
                                 phase.allocator, phase.consume_work);
      if (!created.ok())
        return Result<std::vector<DependencyAtomOutcome>>(created.status());
      kernel = created.take_value();
      for (auto& state : states)
        state.kernel = kernel;
    }
    std::vector<DependencyAtomOutcome> results;
    for (const auto* member : phase.members)
      results.push_back(
          {member->query.output_index,
           states[member->query.output_index].poll_with_kernel(*member)});
    return Result<std::vector<DependencyAtomOutcome>>(std::move(results));
  }
};
}  // namespace
Status register_image_gaussian_blur_with_kernel(OperationRegistry* registry) {
  OperationDefinition op;
  op.key = "image.gaussian_blur_with_kernel";
  auto& traits = op.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  auto& input = traits.input_schema[0];
  input.kind = OperationPortKind::Typed;
  input.semantic_kind = static_cast<std::uint32_t>(SemanticKind::Image);
  input.element_type = static_cast<std::uint32_t>(ElementType::Float32);
  input.rank = 3;
  traits.parameter_schema = {
      {"boundary", OperationParameterType::String, false},
      {"radius", OperationParameterType::Float64, true, true, 0, 64},
      {"sigma", OperationParameterType::Float64, true, true, 0, 64}};
  traits.workspace_bytes = maximum_kernel_bytes;
  traits.outputs.resize(2);
  for (std::uint32_t i = 0; i < 2; ++i) {
    auto& output = traits.outputs[i];
    output.key = i ? "kernel" : "image";
    output.output_element_type = ElementType::Float32;
    output.region_rule = OperationRegionRule::Dependency;
    output.dependency_version = 1;
    output.continuation_bytes = sizeof(State);
    output.maximum_dependency_stages = 2;
    output.failure_delivery = FailureDelivery::PerAtomOutcome;
    output.input_indices =
        i ? std::vector<std::uint32_t>{} : std::vector<std::uint32_t>{0};
  }
  auto& image = traits.outputs[0];
  image.shape_rule = OperationShapeRule::PreserveFirstInput;
  image.output_schema = input;
  image.output_semantic_rule = OperationSemanticRule::PreserveInput;
  auto& kernel = traits.outputs[1];
  kernel.shape_rule = OperationShapeRule::Axes;
  OperationExtent extent;
  extent.source = OperationExtentSource::CeilParameter;
  extent.parameter = "radius";
  extent.multiplier = 2;
  extent.offset = 1;
  kernel.output_axes = {extent, extent};
  traits.joint_contract = 1;
  traits.joint_continuation_bytes = sizeof(Joint);
  traits.joint_workspace_bytes = maximum_kernel_bytes;
  op.validate_dependency = [](const auto&, const auto& parameters) {
    if (parameters.count("boundary")) {
      const auto& boundary = std::get<std::string>(parameters.at("boundary"));
      if (boundary != "zero" && boundary != "clamp")
        return Status{ErrorCode::InvalidArgument,
                      "Gaussian boundary must be zero or clamp"};
    }
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
