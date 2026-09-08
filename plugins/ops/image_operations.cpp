#include "plugin/image_operations.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"

namespace ps::plugin_internal {
namespace {
/** @brief Executes one dense CPU execute_image; host enforces the port
 * contract. */
Result<Value> execute_image(const OperationInvocation& invocation,
                            bool opacity) {
  input_internal::Float32Environment environment;
  if (!environment.active()) {
    return Result<Value>(Status::failure(
        ErrorCode::OperationFailed,
        "image multiplication requires round-to-nearest ties-to-even"));
  }
  const Value& input = invocation.inputs[0];
  float factor = 0;
  std::memcpy(&factor, invocation.inputs[1].bytes().data(), sizeof(factor));
  auto allocated = MutableValue::allocate(
      input.descriptor(), invocation.output_region, invocation.allocator);
  if (!allocated.ok())
    return Result<Value>(allocated.status());
  auto output = allocated.take_value();
  const auto yd = invocation.output_region.dimensions()[0];
  const auto xd = invocation.output_region.dimensions()[1];
  std::size_t destination = 0;
  for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y) {
    if (invocation.cancellation.cancelled())
      return Result<Value>(
          Status::failure(ErrorCode::Cancelled, "image operation cancelled"));
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x) {
      for (std::uint64_t channel = 0; channel < 4; ++channel) {
        const auto offset = input.byte_address({y, x, channel});
        if (!offset.ok())
          return Result<Value>(offset.status());
        float number = 0;
        std::memcpy(&number, input.bytes().data() + offset.value(),
                    sizeof(number));
        if (opacity || channel < 3)
          number *= factor;
        std::memcpy(output.data() + destination, &number, sizeof(number));
        destination += sizeof(number);
      }
    }
  }
  return std::move(output).publish({input_internal::image_facet()});
}
// Validated region coverage makes every sample address representable.
float sample(const Value& value, std::uint64_t y, std::uint64_t x,
             std::uint64_t channel = 0) {
  auto coordinate = value.descriptor().shape.size() == 2
                        ? std::vector<std::uint64_t>{y, x}
                        : std::vector<std::uint64_t>{y, x, channel};
  const auto address = value.byte_address(coordinate);
  if (!address.ok())
    throw std::runtime_error(address.status().message);
  float number = 0;
  std::memcpy(&number, value.bytes().data() + address.value(), sizeof(number));
  return number;
}
std::uint64_t clamp_axis(std::uint64_t coordinate, int tap,
                         std::uint64_t length) {
  if (tap < 0)
    return coordinate < static_cast<std::uint64_t>(-tap)
               ? 0
               : coordinate - static_cast<std::uint64_t>(-tap);
  return std::min(coordinate + static_cast<std::uint64_t>(tap), length - 1);
}
Result<Value> gaussian(const OperationInvocation& invocation) {
  const auto& input = invocation.inputs[0];
  const auto radius = static_cast<int>(
      std::get<std::int64_t>(invocation.parameters.at("radius")));
  const auto sigma = std::get<double>(invocation.parameters.at("sigma"));
  auto made = MutableValue::allocate(
      input.descriptor(), invocation.output_region, invocation.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  auto weights_result = invocation.allocator.allocate((2 * radius + 1) * 8);
  if (!weights_result.ok())
    return Result<Value>(weights_result.status());
  auto weights = weights_result.take_value();
  double total = 0;
  for (int tap = -radius; tap <= radius; ++tap) {
    const double weight =
        std::exp(-static_cast<double>(tap * tap) / (2.0 * sigma * sigma));
    total += weight;
    std::memcpy(weights.data() + (tap + radius) * 8, &weight, 8);
  }
  for (int tap = -radius; tap <= radius; ++tap) {
    double weight = 0;
    std::memcpy(&weight, weights.data() + (tap + radius) * 8, 8);
    weight /= total;
    std::memcpy(weights.data() + (tap + radius) * 8, &weight, 8);
  }
  const auto yd = invocation.output_region.dimensions()[0];
  const auto xd = invocation.output_region.dimensions()[1];
  const auto rows = invocation.input_demands[0].dimensions()[0];
  auto scratch_result =
      invocation.allocator.allocate(rows.extent * xd.extent * 16);
  if (!scratch_result.ok())
    return Result<Value>(scratch_result.status());
  auto scratch = scratch_result.take_value();
  for (std::uint64_t y = rows.offset; y < rows.offset + rows.extent; ++y) {
    if (invocation.cancellation.cancelled())
      return Result<Value>(
          Status::failure(ErrorCode::Cancelled, "Gaussian cancelled"));
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x)
      for (std::uint64_t c = 0; c < 4; ++c) {
        double sum = 0;
        for (int tap = -radius; tap <= radius; ++tap) {
          double weight = 0;
          std::memcpy(&weight, weights.data() + (tap + radius) * 8, 8);
          const double product =
              sample(input, y, clamp_axis(x, tap, input.descriptor().shape[1]),
                     c) *
              weight;
          sum += product;
        }
        const float rounded = static_cast<float>(sum);
        const auto index =
            ((y - rows.offset) * xd.extent + x - xd.offset) * 4 + c;
        std::memcpy(scratch.data() + index * 4, &rounded, 4);
      }
  }
  for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y) {
    if (invocation.cancellation.cancelled())
      return Result<Value>(
          Status::failure(ErrorCode::Cancelled, "Gaussian cancelled"));
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x)
      for (std::uint64_t c = 0; c < 4; ++c) {
        double sum = 0;
        for (int tap = -radius; tap <= radius; ++tap) {
          double weight = 0;
          std::memcpy(&weight, weights.data() + (tap + radius) * 8, 8);
          const auto row = clamp_axis(y, tap, input.descriptor().shape[0]);
          const auto index =
              ((row - rows.offset) * xd.extent + x - xd.offset) * 4 + c;
          float number = 0;
          std::memcpy(&number, scratch.data() + index * 4, 4);
          const double product = number * weight;
          sum += product;
        }
        const float rounded = static_cast<float>(sum);
        const auto index =
            ((y - yd.offset) * xd.extent + x - xd.offset) * 4 + c;
        std::memcpy(output.data() + index * 4, &rounded, 4);
      }
  }
  return std::move(output).publish({input_internal::image_facet()});
}
Result<Value> combine(const OperationInvocation& invocation, bool mask) {
  const auto& foreground = invocation.inputs[0];
  const auto& other = invocation.inputs[1];
  auto made = MutableValue::allocate(
      foreground.descriptor(), invocation.output_region, invocation.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  const auto yd = invocation.output_region.dimensions()[0];
  const auto xd = invocation.output_region.dimensions()[1];
  std::size_t target = 0;
  for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y) {
    if (invocation.cancellation.cancelled())
      return Result<Value>(
          Status::failure(ErrorCode::Cancelled, "image combine cancelled"));
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x) {
      const float factor =
          mask ? sample(other, y, x) : 1.0F - sample(foreground, y, x, 3);
      for (std::uint64_t c = 0; c < 4; ++c) {
        const float attenuated =
            (mask ? sample(foreground, y, x, c) : sample(other, y, x, c)) *
            factor;
        const float number =
            mask ? attenuated : sample(foreground, y, x, c) + attenuated;
        std::memcpy(output.data() + target, &number, 4);
        target += 4;
      }
    }
  }
  return std::move(output).publish({input_internal::image_facet()});
}
}  // namespace

Status register_image_operations(OperationRegistry* registry) {
  for (bool opacity : {false, true}) {
    OperationDefinition operation;
    operation.key = opacity ? "image.opacity" : "image.exposure_gain";
    operation.traits.input_count = 2;
    operation.traits.output_element_type = ElementType::Float32;
    operation.traits.shape_rule = OperationShapeRule::PreserveFirstInput;
    operation.traits.region_rule = OperationRegionRule::Elementwise;
    operation.traits.input_schema = {
        {OperationPortKind::LinearPremultipliedRgbaFloat32, 0, 0},
        {OperationPortKind::Float32Scalar, 0, opacity ? 1.0F : 16.0F}};
    operation.traits.output_schema = operation.traits.input_schema.front();
    operation.callback = [opacity](const OperationInvocation& invocation) {
      return execute_image(invocation, opacity);
    };
    const auto status = registry->register_operation(std::move(operation));
    if (!status.ok())
      return status;
  }
  for (int kind = 0; kind < 3; ++kind) {
    OperationDefinition operation;
    operation.key = kind == 0   ? "image.gaussian_blur"
                    : kind == 1 ? "image.mask"
                                : "image.source_over";
    operation.traits.input_count = kind == 0 ? 1 : 2;
    operation.traits.output_element_type = ElementType::Float32;
    operation.traits.shape_rule = kind == 2
                                      ? OperationShapeRule::MatchAllInputs
                                      : OperationShapeRule::PreserveFirstInput;
    operation.traits.region_rule = kind == 0 ? OperationRegionRule::Halo
                                             : OperationRegionRule::Elementwise;
    operation.traits.input_schema = {
        {OperationPortKind::LinearPremultipliedRgbaFloat32, 0, 0}};
    operation.traits.output_schema = operation.traits.input_schema.front();
    if (kind == 0) {
      operation.traits.parameter_schema = {
          {"radius", OperationParameterType::Int64, true, true, 1, 64},
          {"sigma", OperationParameterType::Float64, true, true, 0.1, 64}};
      operation.traits.halo_radius_parameter = "radius";
      operation.traits.workspace_bytes = 129 * 8;
      operation.traits.workspace_input_multiplier = 1;
      operation.callback = gaussian;
    } else {
      operation.traits.input_schema.push_back(
          {kind == 1 ? OperationPortKind::Float32Mask
                     : OperationPortKind::LinearPremultipliedRgbaFloat32,
           0, 0});
      operation.callback = [kind](const OperationInvocation& invocation) {
        return combine(invocation, kind == 1);
      };
    }
    auto status = registry->register_operation(std::move(operation));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
