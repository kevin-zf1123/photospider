#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "photospider/numeric/arrays.hpp"

namespace ps::numeric {
namespace indexing_detail {
inline Result<WorkflowNode> node(std::uint64_t id, const char* operation,
                                 std::vector<WorkflowInput> inputs,
                                 std::uint32_t axis,
                                 CpuNumericProfile profile) {
  const auto* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!id || axis >= 8 || !suffix)
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               "invalid indexing id/axis/profile",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  return Result<WorkflowNode>(
      WorkflowNode{id,
                   std::string("array.") + operation + suffix,
                   std::move(inputs),
                   {{"axis", static_cast<std::int64_t>(axis)}}});
}
}  // namespace indexing_detail
/** @brief Authors ordered concatenation of 2..256 tensor Results.
 * Each input Result supplies one tensor member at any schema/member key, with
 * a complete sample_shape() including batch axes. Inputs use UInt8, Int64,
 * Float32 or Float64 and share dtype, rank and all non-concatenated extents;
 * each has rank 1..8 and at most 2^40 elements.
 * The result is a photospider.tensor/samples Result with the full concatenated
 * shape as ordinary axes and no facets or batch metadata.
 *
 * The helper only authors node metadata. The CPU Whole program statically
 * validates shape and parameters, then requests active input tensors with
 * Data, Validation and Descriptor roles (role 13) for nonempty work. It
 * publishes the complete output; query projection does not reduce preparation
 * or computation. View proves one affine map across the complete inputs and
 * can join compatible fragments from the same owner. Explicit View reports
 * Domain/Run ViewUnavailable for multiple owners or incompatible geometry.
 * Dense materializes a complete packed output. Concatenate disables
 * cross-run content caching because content does not establish physical
 * viewability; Views retain source storage/resources after context retirement.
 * Empty demand reads no payload or performs sample copying.
 *
 * Inputs are ordered input_0..input_(K-1); the output port is values. This
 * pure, concurrent-safe helper reads no payload and returns owned node
 * metadata. Invalid authoring arguments return
 * InvalidArgument/InvalidDomain/Schema; allocation may throw bad_alloc.
 * Profiles preserve raw element bits.
 */
inline Result<WorkflowNode> concatenate_node(
    std::uint64_t id, std::vector<WorkflowInput> inputs, std::uint32_t axis,
    ArrayLayout layout = ArrayLayout::View,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  if (inputs.size() < 2 || inputs.size() > 256 ||
      (layout != ArrayLayout::View && layout != ArrayLayout::Dense))
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               "invalid concatenate arity/layout",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  auto result = indexing_detail::node(id, "concatenate", std::move(inputs),
                                      axis, profile);
  if (!result.ok())
    return result;
  auto authored = result.take_value();
  authored.parameters["layout"] =
      std::string(layout == ArrayLayout::View ? "view" : "dense");
  return Result<WorkflowNode>(std::move(authored));
}
/** @brief Authors single-axis gather using an Int64 index Result.
 * The source is a single-tensor Result; the index Result has a rank-one tensor
 * [M]. Their complete sample shapes include batch axes, have rank 1..8 and at
 * most 2^40 elements. UInt8, Int64, Float32 and Float64 are supported. The
 * output `values` is a packed photospider.tensor/samples Result with the
 * selected source extent replaced by M and facets/batch metadata dropped; its
 * N*element_size bytes remain owned after context retirement. A nonempty CPU
 * Whole request uses role 13 for source and indices, validates every index and
 * computes the complete output before projection. Repeated indices retain
 * order, and an out-of-range index reports InvalidArgument/InvalidDomain with
 * diagnostic IndexOutOfBounds. Active input edits invalidate recorded output
 * observations. There is no clipping, cast or implicit broadcast. The pure,
 * concurrent-safe helper reads no payload and authors only node metadata.
 * Invalid authoring id/axis/profile returns
 * InvalidArgument/InvalidDomain/Schema; allocation may throw bad_alloc. Source,
 * typed, resource and cancellation failures retain their categories.
 */
inline Result<WorkflowNode> gather_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput indices,
    std::uint32_t axis, CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return indexing_detail::node(
      id, "gather", {std::move(input), std::move(indices)}, axis, profile);
}
/** @brief Authors scatter with base, Int64 indices and matching updates
 * Results. Each Result contributes one tensor member under any key; complete
 * sample_shape() includes batch axes, has rank 1..8 and at most 2^40 elements.
 * UInt8, Int64, Float32 and Float64 are supported. The immutable `values`
 * output retains base shape/dtype as a packed photospider.tensor/samples Result
 * of N*element_size bytes; it has no facets/batch metadata and remains owned
 * after context retirement. Nonempty Whole work requests all three inputs with
 * role 13, validates every index and computes the complete output before
 * projection. Any active input or typed-validation failure can fail the Run.
 * Replacement selects the last update position; aggregates include base, then
 * updates in increasing position. No-hit and replacement copies preserve bits,
 * including signaling NaNs. Sum accumulates exactly and converts once; integer
 * final overflow is OperationFailed/ArithmeticOverflow, attributed to the
 * complete output coordinate as Domain/Run. Minimum/maximum propagate the first
 * NaN and use the NUM-05 signed-zero order. Failures publish no partial Result;
 * source, typed, resource and cancellation failures retain their categories.
 * Active input edits invalidate recorded output observations. The pure,
 * concurrent-safe helper reads no payload and authors node metadata only.
 * Invalid authoring id/axis/profile returns
 * InvalidArgument/InvalidDomain/Schema; allocation may throw bad_alloc.
 */
inline Result<WorkflowNode> scatter_replace_node(
    std::uint64_t id, WorkflowInput base, WorkflowInput indices,
    WorkflowInput updates, std::uint32_t axis,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return indexing_detail::node(
      id, "scatter_replace",
      {std::move(base), std::move(indices), std::move(updates)}, axis, profile);
}
/** @brief Exact sum scatter; see scatter_replace_node for shared contracts. */
inline Result<WorkflowNode> scatter_sum_node(
    std::uint64_t id, WorkflowInput base, WorkflowInput indices,
    WorkflowInput updates, std::uint32_t axis,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return indexing_detail::node(
      id, "scatter_sum",
      {std::move(base), std::move(indices), std::move(updates)}, axis, profile);
}
/** @brief NaN-propagating minimum scatter; shared rules above. */
inline Result<WorkflowNode> scatter_minimum_node(
    std::uint64_t id, WorkflowInput base, WorkflowInput indices,
    WorkflowInput updates, std::uint32_t axis,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return indexing_detail::node(
      id, "scatter_minimum",
      {std::move(base), std::move(indices), std::move(updates)}, axis, profile);
}
/** @brief NaN-propagating maximum scatter; shared rules above. */
inline Result<WorkflowNode> scatter_maximum_node(
    std::uint64_t id, WorkflowInput base, WorkflowInput indices,
    WorkflowInput updates, std::uint32_t axis,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return indexing_detail::node(
      id, "scatter_maximum",
      {std::move(base), std::move(indices), std::move(updates)}, axis, profile);
}
}  // namespace ps::numeric
