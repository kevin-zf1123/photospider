#include "01-numeric/expression_metadata.hpp"

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "common/floating_environment.hpp"

namespace ps::plugin_internal::expression_result {
Status validate_expression_metadata(
    bool lut, const std::vector<TensorMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    const TensorDescriptor& output) {
  if (inputs.empty())
    return {ErrorCode::TypeMismatch, "expression input absent"};
  FloatingEnvironment environment;
  if (!environment.active())
    return {ErrorCode::OperationFailed,
            "expression floating environment unavailable"};
  const auto& input = inputs[0].descriptor;
  if (!lut) {
    if (input.element_type != ElementType::Float64 || input.shape.size() != 1 ||
        input.shape[0] < 1 || input.shape[0] > 256 ||
        output.element_type != ElementType::Float32 ||
        output.shape.size() != 1 || output.shape[0] < 1 ||
        output.shape[0] > 1048576)
      return {ErrorCode::TypeMismatch,
              "expression coefficient/output descriptor outside limits"};
  } else if (inputs.size() != 2 || input.element_type != ElementType::Float32 ||
             output.element_type != ElementType::Float32 ||
             output.shape != input.shape ||
             inputs[1].descriptor.element_type != ElementType::Float32 ||
             inputs[1].descriptor.shape.size() != 1 ||
             inputs[1].descriptor.shape[0] < 2) {
    return {ErrorCode::TypeMismatch,
            "LUT requires Float32 query and at least two table samples"};
  }
  if (lut) {
    const auto& policy = std::get<std::string>(parameters.at("out_of_domain"));
    if (policy != "reject" && policy != "clip")
      return {ErrorCode::InvalidArgument, "LUT policy must be reject or clip"};
  }
  return Status::success();
}
}  // namespace ps::plugin_internal::expression_result
