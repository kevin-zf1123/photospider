#include <utility>

#include "00-foundation/image_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace image_ops;  // NOLINT(build/namespaces)
Result<Value> execute_image(const OperationInvocation& invocation,
                            bool opacity) {
  if (invocation.backend == Backend::Gpu)
    return gpu_image(invocation, opacity ? 1 : 0);
  input_internal::Float32Environment environment;
  if (!environment.active()) {
    return Result<Value>(Status::failure(
        ErrorCode::OperationFailed,
        "image multiplication requires round-to-nearest ties-to-even"));
  }
  const Value& input = invocation.inputs[0];
  float factor = 0;
  std::memcpy(&factor,
              invocation.inputs[1].bytes().data() +
                  invocation.inputs[1].byte_address({0}).value(),
              sizeof(factor));
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
  return std::move(output).publish(invocation.inputs[0].facets());
}
}  // namespace
Status register_image_opacity(OperationRegistry* registry) {
  OperationDefinition operation;
  operation.traits.output_semantic_rule = OperationSemanticRule::PreserveInput;
  operation.traits.supports_gpu = operation.traits.allows_cpu_fallback = true;
  operation.key = "image.opacity";
  operation.traits.input_count = 2;
  operation.traits.output_element_type = ElementType::Float32;
  operation.traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  operation.traits.region_rule = OperationRegionRule::Elementwise;
  operation.traits.input_schema = {{OperationPortKind::RgbaFloat32, 0, 0},
                                   {OperationPortKind::Float32Scalar, 0, 1.0F}};
  operation.traits.output_schema = operation.traits.input_schema.front();
  operation.callback = [](const OperationInvocation& invocation) {
    return execute_image(invocation, true);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
