#include <utility>

#include "00-foundation/image_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace image_ops;  // NOLINT(build/namespaces)
Result<Value> combine(const OperationInvocation& invocation, bool mask) {
  if (invocation.backend == Backend::Gpu)
    return gpu_image(invocation, mask ? 3 : 4);
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
  return std::move(output).publish(invocation.inputs[0].facets());
}
}  // namespace
Status register_image_source_over(OperationRegistry* registry) {
  constexpr int kind = 2;
  OperationDefinition operation;
  operation.traits.output_semantic_rule = OperationSemanticRule::PreserveInput;
  operation.traits.supports_gpu = operation.traits.allows_cpu_fallback = true;
  operation.key = kind == 0   ? "image.gaussian_blur"
                  : kind == 1 ? "image.mask"
                              : "image.source_over";
  operation.traits.input_count = kind == 0 ? 1 : 2;
  operation.traits.output_element_type = ElementType::Float32;
  operation.traits.shape_rule = kind == 2
                                    ? OperationShapeRule::MatchAllInputs
                                    : OperationShapeRule::PreserveFirstInput;
  operation.traits.region_rule =
      kind == 0 ? OperationRegionRule::Halo : OperationRegionRule::Elementwise;
  operation.traits.input_schema = {{OperationPortKind::RgbaFloat32, 0, 0}};
  operation.traits.output_schema = operation.traits.input_schema.front();

  operation.traits.input_schema.push_back({kind == 1
                                               ? OperationPortKind::Float32Mask
                                               : OperationPortKind::RgbaFloat32,
                                           0, 0});
  operation.callback = [](const OperationInvocation& invocation) {
    return combine(invocation, kind == 1);
  };

  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
