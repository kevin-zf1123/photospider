#include <limits>
#include <utility>

#include "00-foundation/image_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace image_ops;  // NOLINT(build/namespaces)
Result<Value> brush_circle(const OperationInvocation& invocation) {
  if (invocation.backend == Backend::Gpu)
    return gpu_image(invocation, 7);
  const auto& input = invocation.inputs[0];
  float args[7];
  for (std::size_t i = 0; i < 7; ++i)
    std::memcpy(&args[i],
                invocation.inputs[i + 1].bytes().data() +
                    invocation.inputs[i + 1].byte_address({0}).value(),
                4);
  const double cx = args[0], cy = args[1], radius = args[2];
  const float alpha = args[6], remaining = 1.0F - alpha;
  auto made = MutableValue::allocate(
      input.descriptor(), invocation.output_region, invocation.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  const auto yd = invocation.output_region.dimensions()[0],
             xd = invocation.output_region.dimensions()[1];
  std::size_t target = 0;
  for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y) {
    if (invocation.cancellation.cancelled())
      return Result<Value>(
          Status::failure(ErrorCode::Cancelled, "brush cancelled"));
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x) {
      const double dx = static_cast<double>(x) + .5 - cx,
                   dy = static_cast<double>(y) + .5 - cy;
      const bool inside = dx * dx + dy * dy <= radius * radius;
      for (std::uint64_t c = 0; c < 4; ++c) {
        float number = sample(input, y, x, c);
        if (inside) {
          const float source = c == 3 ? alpha : args[3 + c] * alpha;
          const float back = number * remaining;
          number = source + back;
        }
        std::memcpy(output.data() + target, &number, 4);
        target += 4;
      }
    }
  }
  return std::move(output).publish(input.facets());
}
}  // namespace
Status register_image_brush_circle(OperationRegistry* registry) {
  OperationDefinition brush;
  brush.key = "image.brush_circle";
  brush.traits.outputs[0].output_semantic_rule =
      OperationSemanticRule::PreserveInput;
  brush.callback = brush_circle;
  auto& traits = brush.traits;
  traits.supports_gpu = traits.allows_cpu_fallback = true;
  traits.workspace_input_multiplier = 1;
  traits.input_count = 8;
  traits.outputs[0].output_element_type = ElementType::Float32;
  traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  traits.outputs[0].region_rule = OperationRegionRule::Elementwise;
  traits.outputs[0].output_schema.kind = OperationPortKind::RgbaFloat32;
  const float maximum = std::numeric_limits<float>::max();
  traits.input_schema = {traits.outputs[0].output_schema,
                         {OperationPortKind::Float32Scalar, -maximum, maximum},
                         {OperationPortKind::Float32Scalar, -maximum, maximum},
                         {OperationPortKind::Float32Scalar,
                          std::numeric_limits<float>::min(), maximum},
                         {OperationPortKind::Float32Scalar, -maximum, maximum},
                         {OperationPortKind::Float32Scalar, -maximum, maximum},
                         {OperationPortKind::Float32Scalar, -maximum, maximum},
                         {OperationPortKind::Float32Scalar, 0, 1}};
  return registry->register_operation(std::move(brush));
}
}  // namespace ps::plugin_internal
