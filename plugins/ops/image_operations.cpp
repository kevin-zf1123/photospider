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
  std::vector<std::uint8_t> bytes = input.bytes();
  for (std::size_t offset = 0; offset < bytes.size(); offset += 16) {
    if (offset % 16384 == 0 && invocation.cancellation.cancelled()) {
      return Result<Value>(
          Status::failure(ErrorCode::Cancelled, "image operation cancelled"));
    }
    for (std::size_t channel = 0; channel < (opacity ? 4U : 3U); ++channel) {
      float value = 0;
      std::memcpy(&value, bytes.data() + offset + channel * 4, sizeof(value));
      const float product = value * factor;
      std::memcpy(bytes.data() + offset + channel * 4, &product,
                  sizeof(product));
    }
  }
  return Value::create(input.descriptor(), input.region(), input.layout(),
                       std::move(bytes), {input_internal::image_facet()});
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
