#include <algorithm>
#include <utility>

#include "00-foundation/image_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace image_ops;  // NOLINT(build/namespaces)
Result<Value> downsample(const OperationInvocation& invocation) {
  if (invocation.backend == Backend::Gpu)
    return gpu_image(
        invocation,
        invocation.inputs[0].descriptor().shape.size() == 3 ? 5 : 6);
  const auto& input = invocation.inputs[0];
  const auto factor = static_cast<std::uint64_t>(
      std::get<std::int64_t>(invocation.parameters.at("factor")));
  auto descriptor = input.descriptor();
  for (std::size_t a = 0; a < 2; ++a)
    descriptor.shape[a] =
        descriptor.shape[a] / factor + (descriptor.shape[a] % factor != 0);
  auto made = MutableValue::allocate(descriptor, invocation.output_region,
                                     invocation.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  const auto yd = invocation.output_region.dimensions()[0],
             xd = invocation.output_region.dimensions()[1];
  const std::uint64_t channels = descriptor.shape.size() == 3 ? 4 : 1;
  std::size_t target = 0;
  for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y) {
    if (invocation.cancellation.cancelled())
      return Result<Value>(
          Status::failure(ErrorCode::Cancelled, "downsample cancelled"));
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x) {
      const auto y0 = y * factor, x0 = x * factor;
      const auto h = std::min(factor, input.descriptor().shape[0] - y0),
                 w = std::min(factor, input.descriptor().shape[1] - x0);
      for (std::uint64_t c = 0; c < channels; ++c) {
        double sum = 0;
        for (std::uint64_t row = y0; row < y0 + h; ++row)
          for (std::uint64_t col = x0; col < x0 + w; ++col)
            sum += sample(input, row, col, c);
        const float number =
            static_cast<float>(sum / static_cast<double>(h * w));
        std::memcpy(output.data() + target, &number, 4);
        target += 4;
      }
    }
  }
  return std::move(output).publish(input.facets());
}
}  // namespace
Status register_mask_downsample_box(OperationRegistry* registry) {
  OperationDefinition operation;
  operation.traits.outputs[0].output_semantic_rule =
      OperationSemanticRule::PreserveInput;
  operation.traits.supports_gpu = operation.traits.allows_cpu_fallback = true;
  operation.key = "mask.downsample_box";
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.outputs[0].output_element_type = ElementType::Float32;
  traits.outputs[0].shape_rule = OperationShapeRule::Shrink;
  traits.outputs[0].region_rule = OperationRegionRule::Shrink;
  traits.outputs[0].spatial_factor_parameter = "factor";
  traits.parameter_schema = {
      {"factor", OperationParameterType::Int64, true, true, 1, 16}};
  traits.outputs[0].output_schema.kind = OperationPortKind::Float32Mask;
  traits.input_schema = {traits.outputs[0].output_schema};
  operation.callback = downsample;
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
