#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "photospider/ops/numeric/color_description.hpp"
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
                                 const TensorDescription& input_description,
                                 const TensorDescription& output_description,
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
  auto input_group = resolve_color_tuple(input_description);
  auto output_group = resolve_color_tuple(output_description);
  if (!input_group.ok() || !output_group.ok())
    return Answer(!input_group.ok() ? input_group.status()
                                    : output_group.status());
  if (input_group.value().interpretation.model !=
          output_group.value().interpretation.model ||
      input_group.value().components.size() != 3 ||
      output_group.value().components.size() != 3 ||
      input_group.value().alpha || output_group.value().alpha)
    return Answer(invalid("LUT3D requires same-model three-component colors"));
  for (const auto* group : {&input_group.value(), &output_group.value()})
    for (const auto& component : group->components)
      if (component.unit == "rational_pi")
        return Answer(invalid("LUT3D requires floating hue coordinates"));
  auto source = tensor_description_parameter(input_description);
  if (!source.ok())
    return Answer(source.status());
  auto destination = tensor_description_parameter(output_description);
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
 * output use this explicit description for caller numerical preconditions.
 * The descriptions may differ within the same model.
 * @param options Explicit output dtype, Reject/Clamp policy and CPU profile.
 * @return Owned node metadata or InvalidArgument/InvalidDomain/Schema for
 * invalid authoring choices. Allocation may throw bad_alloc.
 * @note Pure/concurrent-safe helper. Output `values` is an immutable packed
 * Result using `photospider.tensor` v1/member `samples`, complete input shape,
 * selected Float32/64 dtype, atomic_trailing_axes=1.
 * Each selected output tile has one fixed CPU invocation. Reads close its query
 * region, complete table and six axis endpoints on each source's own grid.
 * Exact positive-weight complete vertices alone enter arithmetic, at most
 * eight. Upstream failures propagate through the full table dependency.
 * The output carries no color description or input batch partition.
 * Empty emits no tasks. Each invocation owns preallocated arithmetic/knots
 * scratch and borrows affine inputs; retained output survives context teardown.
 * Preparation is immutable and shareable across concurrent Runs.
 *
 * Axis finiteness, step consistency, strict reconstructed order, original query
 * legality, contributing table color legality, reject-domain membership and
 * finite output are unchecked caller preconditions. Clamp acts on valid colors.
 * Structural signature checks occur before invocation. Cancellation,
 * capacity and upstream failures retain operational errors; successful call
 * publication follows all required task completions in retirement order.
 * Both descriptions are canonical logical tuples (channel_axis=0). Native
 * CIELAB/CIELCh use l = L-star / 100; axes, table samples, validation points
 * and tolerances must already use that scale. No implicit conversion occurs.
 */
inline Result<WorkflowNode> apply_lut3d_trilinear_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput table,
    WorkflowInput axis, ElementType input_type,
    const TensorDescription& input_description,
    const TensorDescription& output_description, Lut3dOptions options = {}) {
  return lut3d_detail::node(id, "trilinear", std::move(input), std::move(table),
                            std::move(axis), input_type, input_description,
                            output_description, options);
}
/** @brief Main-diagonal tetrahedral LUT3D interpolation, at most four vertices.
 * Shares apply_lut3d_trilinear_node's Result inputs, schema, descriptions,
 * explicit configuration, caller preconditions, ownership, errors and resource
 * contract. Exact local coordinates sort descending, with axis 0/1/2 breaking
 * ties; the four vertices are 000, e_a, e_a+e_b and 111. Exact positive weights
 * determine mathematical participation; zero-weight split vertices remain
 * covered by the upstream table dependency. No method parameter/default is
 * implied.
 */
inline Result<WorkflowNode> apply_lut3d_tetrahedral_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput table,
    WorkflowInput axis, ElementType input_type,
    const TensorDescription& input_description,
    const TensorDescription& output_description, Lut3dOptions options = {}) {
  return lut3d_detail::node(id, "tetrahedral", std::move(input),
                            std::move(table), std::move(axis), input_type,
                            input_description, output_description, options);
}
}  // namespace ps::numeric
