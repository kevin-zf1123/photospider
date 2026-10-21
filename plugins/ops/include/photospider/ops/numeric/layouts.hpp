#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/ops/numeric/diagnostics.hpp"

namespace ps::numeric {
namespace layout_detail {
inline Result<WorkflowNode> node(std::uint64_t id, const char* operation,
                                 std::vector<WorkflowInput> inputs,
                                 const char* parameter,
                                 const std::vector<std::uint64_t>& values,
                                 bool permutation, CpuNumericProfile profile) {
  const auto invalid = [](const char* message) {
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               message,
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  if (!id || values.empty() || values.size() > 8)
    return invalid("array transform id/rank outside bounds");
  std::string encoded;
  std::uint64_t product = 1;
  unsigned used = 0;
  for (auto value : values) {
    if (permutation) {
      if (value >= values.size() || (used & (1U << value)))
        return invalid("array permutation must contain each input axis once");
      used |= 1U << value;
    } else {
      if (!value || value > (UINT64_C(1) << 40) / product)
        return invalid("array transform shape exceeds 2^40 elements");
      product *= value;
    }
    if (!encoded.empty())
      encoded += ',';
    encoded += std::to_string(value);
  }
  const auto* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!suffix)
    return invalid("unsupported transform profile");
  return Result<WorkflowNode>(
      WorkflowNode{id,
                   std::string("array.") + operation + suffix,
                   std::move(inputs),
                   {{parameter, std::move(encoded)}}});
}
}  // namespace layout_detail
/** @brief Authors a row-major logical reshape with an explicit target shape.
 * Source is a single-tensor Result with any schema/member key, rank 1..8,
 * positive extents and <=2^40 elements. Target count must match. All seven
 * numeric dtypes are preserved bit-for-bit, including signaling NaN payloads.
 * Output values uses photospider.tensor/samples with ordinary target axes.
 * Each selected output tile becomes one fixed CPU task with an independent
 * dense allocation. Source reads are the tiles touched by the global logical
 * mapping. Empty selection emits no task. Affine source strides may be negative
 * or zero; output owners remain independent of source owners.
 * Prepare validates all fixed views and source membership. Runtime uses only
 * bounded stack coordinates and a small byte buffer, without scratch payload.
 * Pure/concurrent-safe helpers return owned node metadata and read no payload.
 * Invalid static parameters return InvalidArgument; incompatible metadata
 * returns TypeMismatch. Allocation may throw bad_alloc. Cancellation/resource
 * failure prevents successful call publication. Accepted work retains owners
 * through drain; successful Results outlive execution context destruction.
 */
inline Result<WorkflowNode> reshape_node(
    std::uint64_t id, WorkflowInput input,
    const std::vector<std::uint64_t>& shape,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return layout_detail::node(id, "reshape", {std::move(input)}, "shape", shape,
                             false, profile);
}
/** @brief Authors an axis permutation; output axis j is input permutation[j].
 * permutation must contain 0..rank-1 exactly once; Compiler checks input rank.
 * Raw-bit, error and owner rules are the same as reshape_node.
 */
inline Result<WorkflowNode> transpose_node(
    std::uint64_t id, WorkflowInput input,
    const std::vector<std::uint64_t>& permutation,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return layout_detail::node(id, "transpose", {std::move(input)}, "permutation",
                             permutation, true, profile);
}
/** @brief Authors a dynamic slice with static positive per-axis counts.
 * starts and steps are Int64[rank] Result inputs. Counts cannot exceed source
 * extents. Each task declares source tiles reachable by any legal controls,
 * plus the complete starts and used steps vectors. Runtime directly computes
 * selected coordinates with widened arithmetic; complete-domain bounds are
 * unchecked caller premises.
 * Starts are nonnegative absolute indices; used steps are nonzero. Singleton
 * axes ignore step values; an all-singleton slice excludes the steps payload
 * and producer while still checking its static schema. Empty reads nothing.
 * No control-domain diagnostic is promised. Controls never alter Plan topology.
 * Output allocation, ownership and concurrency match reshape.
 */
inline Result<WorkflowNode> slice_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput starts,
    WorkflowInput steps, const std::vector<std::uint64_t>& counts,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return layout_detail::node(
      id, "slice", {std::move(input), std::move(starts), std::move(steps)},
      "counts", counts, false, profile);
}
}  // namespace ps::numeric
