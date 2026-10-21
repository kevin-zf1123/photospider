#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/ops/numeric/diagnostics.hpp"

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
 * Inputs each contain one tensor with any schema/member key; complete sample
 * shapes include batch axes, have rank 1..8 and at most 2^40 elements. UInt8,
 * Int64, Float32 and Float64 are supported. Dtype/rank/non-axis extents match.
 * Output values uses photospider.tensor/samples with ordinary concatenated
 * axes. Each selected output tile has one fixed CPU task and an independent
 * dense allocation. Reads include only tiles intersecting that output's input
 * slabs. Empty selection emits no task. Raw bits, including sNaN, are
 * preserved. Prepare verifies fixed views. Runtime uses bounded stack state and
 * no scratch payload. Affine source views may be unaligned, negative-stride or
 * zero-stride. This pure/concurrent-safe helper returns owned metadata and
 * reads no payload. Invalid authoring parameters return InvalidArgument;
 * incompatible schemas return TypeMismatch. Allocation may throw bad_alloc.
 * Accepted work retains owners through cancellation drain; successful Results
 * outlive the context.
 */
inline Result<WorkflowNode> concatenate_node(
    std::uint64_t id, std::vector<WorkflowInput> inputs, std::uint32_t axis,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  if (inputs.size() < 2 || inputs.size() > 256)
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               "invalid concatenate arity",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  return indexing_detail::node(id, "concatenate", std::move(inputs), axis,
                               profile);
}
/** @brief Authors single-axis gather with a dynamic Int64[M] index Result.
 * Source rank is 1..8, with UInt8/Int64/Float32/Float64 dtype. Complete sample
 * shapes include batch axes and have <=2^40 elements. Output values uses
 * photospider.tensor/samples with ordinary axes, replacing axis extent by M.
 * A nonempty selection emits one shared index-construction task and one task
 * per output tile. Every index must be in the source axis, including unselected
 * output positions, as an unchecked caller premise.
 * The shared task writes a private dense Int64[M,2] normalized table (16*M
 * bytes), with no arithmetic scratch. Consumers declare the non-axis output
 * region and complete source axis, then copy only selected output tiles.
 * Duplicate indices preserve output order and source bits, including sNaN.
 * Empty emits no tasks. Preparation proves static view signatures; normalized
 * positions are trusted without runtime bounds checks. Ownership, errors and
 * helper concurrency follow concatenate_node. Runtime topology remains fixed.
 */
inline Result<WorkflowNode> gather_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput indices,
    std::uint32_t axis, CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return indexing_detail::node(
      id, "gather", {std::move(input), std::move(indices)}, axis, profile);
}
/** @brief Authors scatter with base, Int64[M] indices and matching updates.
 * Base/updates use UInt8/Int64/Float32/Float64, rank 1..8 and <=2^40 elements.
 * Updates match base except axis extent M. Each Result has one tensor at any
 * member key; complete sample shapes include batch axes. Output values uses
 * photospider.tensor/samples with ordinary base axes and independent tiles.
 * A shared fixed task trusts every index and stably groups (target,position)
 * pairs in a private Int64[M,2] tile. It uses that 16*M-byte tile and one
 * 16*M-byte scratch buffer for eight radix passes. Consumers read the base
 * tile region and all update positions for its non-axis region. They use
 * bounded searches; normalized update positions are unchecked premises.
 * No-hit copies base bits; Replace copies the last matching update bits.
 * Aggregates include base then matching updates in original position order.
 * Sum uses exact accumulation and one final conversion; representability of
 * each computed integer output is an unchecked caller precondition. Minimum/
 * maximum preserve first-NaN and signed-zero rules. Aggregate state is private
 * preallocated scratch. Empty emits no tasks. Mutable state is never shared
 * across Runs. Index legality is unchecked; resource and cancellation failures
 * prevent successful call publication. Successful
 * Results own storage beyond context lifetime. Helpers are
 * pure/concurrent-safe, return owned metadata and may throw bad_alloc, as
 * concatenate_node describes.
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
