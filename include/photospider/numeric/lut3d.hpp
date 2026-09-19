#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "photospider/data/color_array.hpp"
#include "photospider/numeric/curves.hpp"

namespace ps::numeric {
/** @brief Authoring choices shared by the independent 3D interpolation methods.
 * Descriptions and dynamic axis data remain explicit constructor arguments.
 */
struct Lut3dOptions {
  /** @brief Defaults to the explicitly supplied input dtype hint. */
  std::optional<ElementType> dtype;
  /** @brief Reject by default; Clamp is the other supported policy. */
  CurveDomain out_of_domain = CurveDomain::Reject;
  CpuNumericProfile profile = CpuNumericProfile::Strict;
};
namespace lut3d_detail {
inline Result<WorkflowNode> node(std::uint64_t id, const char* method,
                                 WorkflowInput input, WorkflowInput table,
                                 WorkflowInput axis, ElementType input_type,
                                 const ColorArrayDescriptor& input_description,
                                 const ColorArrayDescriptor& output_description,
                                 const Lut3dOptions& options) {
  using Answer = Result<WorkflowNode>;
  const auto invalid = [](const char* message) {
    return Status{ErrorCode::InvalidArgument,
                  message,
                  FailureReason::InvalidDomain,
                  {FailureOrigin::Schema, FailureScope::Unspecified}};
  };
  const auto dtype = options.dtype.value_or(input_type);
  const auto* suffix = options.profile == CpuNumericProfile::Strict ? "_strict"
                       : options.profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : options.profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!id || !suffix ||
      (input_type != ElementType::Float32 &&
       input_type != ElementType::Float64) ||
      (dtype != ElementType::Float32 && dtype != ElementType::Float64) ||
      (options.out_of_domain != CurveDomain::Reject &&
       options.out_of_domain != CurveDomain::Clamp))
    return Answer(invalid("invalid LUT3D id/dtype/domain/profile"));
  if (input_description.model != output_description.model ||
      input_description.model == ColorModel::Cmyk ||
      input_description.association != ColorAssociation::None ||
      output_description.association != ColorAssociation::None ||
      input_description.source_layout != ColorSourceLayout::Interleaved ||
      output_description.source_layout != ColorSourceLayout::Interleaved)
    return Answer(invalid(
        "LUT3D requires same-model three-component interleaved colors"));
  auto source = color_array_parameter(input_description);
  if (!source.ok())
    return Answer(source.status());
  auto destination = color_array_parameter(output_description);
  if (!destination.ok())
    return Answer(destination.status());
  return Answer(WorkflowNode{
      id,
      std::string("curve.apply_lut3d_") + method + suffix,
      {std::move(input), std::move(table), std::move(axis)},
      {{"input_color_description", source.take_value()},
       {"output_color_description", destination.take_value()},
       {"dtype",
        std::string(dtype == ElementType::Float32 ? "float32" : "float64")},
       {"out_of_domain",
        std::string(options.out_of_domain == CurveDomain::Clamp ? "clamp"
                                                                : "reject")}}});
}
}  // namespace lut3d_detail
/** @brief Exact joint three-axis interpolation with trilinear weights.
 * @param input Float32/64 [...,3], rank 2..8, at most 2^40 logical values.
 * @param table Float32/64 [N0,N1,N2,3], each Ni=2..256.
 * @param axis Float64 [3,3], each row [start,end,step]; each axis independently
 * ascends/descends under the globally checked CRV-05 rounded grid rules.
 * @param input_type Authoring hint for default output dtype; Compiler verifies
 * actual edge metadata.
 * @param input_description Interpretation of lookup coordinates.
 * @param output_description Same model as input, without alpha/CMYK/split hue;
 * the table stores this description and output retains it. Other color-space
 * fields may differ. Existing attached ColorArray descriptions must match.
 * @return Node with explicit statics and output values, or
 * InvalidArgument/InvalidDomain/Schema for invalid authoring choices.
 * @note Pure and thread-safe; allocation may throw bad_alloc. Runtime observes
 * complete colors, validates original input before clamp and reads only exact
 * nonzero-weight full table vertices (at most eight). Global axis and typed
 * Validation remain dependencies. Every profile correctly rounds each complete
 * weighted sum once; unnormalized hues and supported finite extensions remain.
 * Malformed axes/source domains fail OperationFailed/InvalidDomain; final
 * overflow fails ArithmeticOverflow. Empty reads no data. Host work, capacity,
 * cancellation, atomic failures and immutable output ownership apply. No
 * implicit transfer/color conversion or full-table copy occurs.
 */
inline Result<WorkflowNode> apply_lut3d_trilinear_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput table,
    WorkflowInput axis, ElementType input_type,
    const ColorArrayDescriptor& input_description,
    const ColorArrayDescriptor& output_description, Lut3dOptions options = {}) {
  return lut3d_detail::node(id, "trilinear", std::move(input), std::move(table),
                            std::move(axis), input_type, input_description,
                            output_description, options);
}
/** @brief Main-diagonal tetrahedral LUT3D interpolation, at most four vertices.
 * Shares apply_lut3d_trilinear_node's interface, validation and ownership.
 * Exact local coordinates sort descending, with axis 0/1/2 breaking ties;
 * zero-weight vertices are not read. No method parameter/default is implied.
 */
inline Result<WorkflowNode> apply_lut3d_tetrahedral_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput table,
    WorkflowInput axis, ElementType input_type,
    const ColorArrayDescriptor& input_description,
    const ColorArrayDescriptor& output_description, Lut3dOptions options = {}) {
  return lut3d_detail::node(id, "tetrahedral", std::move(input),
                            std::move(table), std::move(axis), input_type,
                            input_description, output_description, options);
}
}  // namespace ps::numeric
