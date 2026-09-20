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
/** @brief Authors ordered concatenation of 2..256 same-dtype, same-rank arrays.
 * Compiler checks non-axis extents and the checked axis sum/count <=2^40.
 * View retains immutable source owners in separate fragments; Dense packs each
 * requested rectangle. Only hit ports have Data/typed Validation support.
 * Inputs are ordered input_0..input_(K-1); output is values with empty facets.
 * Helpers access no payload and return owned node metadata. Invalid arguments
 * return InvalidArgument/InvalidDomain/Schema; allocation may throw bad_alloc.
 * Calls are pure and concurrent-safe. All profiles preserve raw element bits.
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
/** @brief Authors dense single-axis gather with dynamic Int64[M] indices.
 * Source and result share dtype/rank; result axis length is M. Only indices at
 * requested axis positions are read/validated. Repeated indices retain exact
 * shared source dependencies. Invalid observed index returns IndexOutOfBounds
 * as InvalidArgument/InvalidDomain; no clipping, casts or implicit broadcasts.
 * Ownership, errors and concurrency follow concatenate_node; output is values.
 */
inline Result<WorkflowNode> gather_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput indices,
    std::uint32_t axis, CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return indexing_detail::node(
      id, "gather", {std::move(input), std::move(indices)}, axis, profile);
}
/** @brief Scatter rules with base, Int64[M] indices and matching updates.
 * Output values preserves base shape/dtype and has empty facets. Every nonempty
 * request validates all indices before reading base/updates. Replacement reads
 * only the last matching update; aggregates include base then increasing j.
 * No-hit paths preserve base bits including sNaN. Sum rounds the exact total
 * once; integer final overflow fails with ArithmeticOverflow. Minimum/maximum
 * propagate the first NaN and use signed-zero numerical order. Outputs are
 * immutable owned dense rectangles and outlive the context. Source/resource/
 * cancellation failures retain their categories; no partial result is returned.
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
