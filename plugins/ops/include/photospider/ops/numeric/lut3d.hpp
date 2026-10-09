#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "photospider/data/color_array.hpp"
#include "photospider/ops/numeric/curves.hpp"

namespace ps::numeric {
/** @brief Authoring choices shared by the independent 3D interpolation methods.
 * `dtype` defaults from the explicit authoring input_type hint; it is not
 * inferred from the bound Result. Descriptions and axis data remain explicit.
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
 * @param id Nonzero workflow node id.
 * @param input Result with one tensor member under any schema id/version/key;
 * Float32/64 `sample_shape()` [...,3], rank 2..8, product <=2^40.
 * @param table Result tensor Float32/64 [N0,N1,N2,3], each Ni=2..256.
 * @param axis Result tensor Float64 [3,3]; rows are [start,end,step].
 * All shapes use complete `sample_shape()` including batch axes. Every input
 * schema may have an arbitrary id/member key and has one tensor member.
 * @param input_type Authoring dtype hint used only for default output dtype;
 * Compiler independently validates actual Result edge metadata.
 * @param input_description Interpretation of lookup coordinates.
 * @param output_description Same supported model; no alpha/CMYK. Table and
 * output use this description, retained on the output ColorArray v1 facet.
 * The descriptions may differ within the same model.
 * @param options Explicit output dtype, Reject/Clamp policy and CPU profile.
 * @return Owned node metadata or InvalidArgument/InvalidDomain/Schema for
 * invalid authoring choices. Allocation may throw bad_alloc.
 * @note Pure/concurrent-safe helper. Output `values` is an immutable packed
 * Result using `photospider.tensor` v1/member `samples`, complete input shape,
 * selected Float32/64 dtype, ColorArray v1 facet and atomic_trailing_axes=1.
 * Whole execution requests input/table/axis with Data, Validation and
 * Descriptor (role 13). Callback validates all axes, then original query colors
 * before clamp, then table mathematics. Exact positive-weight full-color
 * vertices alone enter the formula, at most eight; typed/upstream validation
 * still covers zero-weight values. The output carries source ObjectId
 * association and full certified coverage in global coordinates. Dirty mapping
 * follows recorded demand; Empty reads no payload. The output transaction is
 * all-or-nothing.
 *
 * `UniformAxis` stores Root-owned Float64 grids costing 8*sum(Ni) bytes.
 * Input/table access uses authorized zero-copy Root windows; no dense table
 * copy is required. Full packed output and exact workspace use managed budgets.
 * Cancellation/resource failure releases unpublished state; output ownership
 * survives context teardown. Numeric failures have Run scope. No implicit color
 * conversion or lightness-unit conversion is performed; ColorArray v1 retains
 * its existing CIELAB lightness interpretation.
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
 * Shares apply_lut3d_trilinear_node's Result inputs, schema, descriptions,
 * output facet, validation order, ownership, errors and resource contract.
 * Exact local coordinates sort descending, with axis 0/1/2 breaking ties; the
 * four vertices are 000, e_a, e_a+e_b and 111. Exact positive weights determine
 * mathematical participation; zero-weight split vertices remain covered by
 * typed/upstream validation. No method parameter/default is implied.
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
