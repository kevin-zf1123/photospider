#include "plugin/image_operations.hpp"

#include <cstring>
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
  return Status::success();
}
}  // namespace ps::plugin_internal
