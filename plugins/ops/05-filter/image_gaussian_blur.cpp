#include <utility>

#include "00-foundation/image_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace image_ops;  // NOLINT(build/namespaces)
Result<Value> gaussian(const OperationInvocation& invocation) {
  if (invocation.backend == Backend::Gpu)
    return gpu_image(invocation, 2);
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
  return std::move(output).publish(invocation.inputs[0].facets());
}
}  // namespace
Status register_image_gaussian_blur(OperationRegistry* registry) {
  OperationDefinition operation;
  operation.traits.outputs[0].output_semantic_rule =
      OperationSemanticRule::PreserveInput;
  operation.traits.supports_gpu = operation.traits.allows_cpu_fallback = true;
  operation.key = "image.gaussian_blur";
  operation.traits.input_count = 1;
  operation.traits.outputs[0].output_element_type = ElementType::Float32;
  operation.traits.outputs[0].shape_rule =
      OperationShapeRule::PreserveFirstInput;
  operation.traits.outputs[0].region_rule = OperationRegionRule::Halo;
  operation.traits.input_schema = {{OperationPortKind::RgbaFloat32, 0, 0}};
  operation.traits.outputs[0].output_schema =
      operation.traits.input_schema.front();

  operation.traits.parameter_schema = {
      {"radius", OperationParameterType::Int64, true, true, 1, 64},
      {"sigma", OperationParameterType::Float64, true, true, 0.1, 64}};
  operation.traits.outputs[0].halo_radius_parameter = "radius";
  operation.traits.workspace_bytes = 129 * 8;
  operation.traits.workspace_input_multiplier = 1;
  operation.callback = gaussian;

  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
