#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"

namespace ps::numeric {
/** @brief Complete-output immutable view or packed-copy policy. */
enum class TransformLayout { Auto, View, Dense };
namespace layout_detail {
inline Result<WorkflowNode> node(std::uint64_t id, const char* operation,
                                 std::vector<WorkflowInput> inputs,
                                 const char* parameter,
                                 const std::vector<std::uint64_t>& values,
                                 bool permutation, TransformLayout layout,
                                 CpuNumericProfile profile) {
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
  const auto* layout_name = layout == TransformLayout::Auto    ? "auto"
                            : layout == TransformLayout::View  ? "view"
                            : layout == TransformLayout::Dense ? "dense"
                                                               : nullptr;
  const auto* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!layout_name || !suffix)
    return invalid("unsupported transform layout/profile");
  return Result<WorkflowNode>(WorkflowNode{
      id,
      std::string("array.") + operation + suffix,
      std::move(inputs),
      {{parameter, std::move(encoded)}, {"layout", std::string(layout_name)}}});
}
}  // namespace layout_detail
/** @brief Authors a row-major logical reshape with explicit target shape.
 * Compiler checks equal positive element counts <=2^40, rank 1..8 and source
 * dtype. Output values preserves UInt8/Int64/Float32/Float64 bits, with empty
 * facets. No payload is accessed by this pure, concurrent-safe helper.
 * Every nonempty demand reads and validates the complete active input, then
 * publishes the complete output before projection. Any active input edit
 * invalidates the complete output; upstream/typed/domain failures are Run-wide.
 * Auto uses one affine owner for the complete output or allocates a full packed
 * copy. View reports Domain/Run InvalidArgument/ViewUnavailable if the complete
 * input/output cannot use one affine owner. Compatible same-owner fragments
 * may join; multiple owners require Auto/Dense collection. Views retain their
 * immutable source storage/resources after context destruction. Dense owns
 * N*dtype_size output bytes, including for a sparse downstream request.
 * Resource/cancellation failures never trigger Auto fallback.
 * Returns owned node metadata; malformed authoring arguments return
 * InvalidArgument/InvalidDomain/Schema, allocation may throw bad_alloc.
 */
inline Result<WorkflowNode> reshape_node(
    std::uint64_t id, WorkflowInput input,
    const std::vector<std::uint64_t>& shape,
    TransformLayout layout = TransformLayout::Auto,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return layout_detail::node(id, "reshape", {std::move(input)}, "shape", shape,
                             false, layout, profile);
}
/** @brief Authors an axis permutation; output axis j is input permutation[j].
 * permutation must contain 0..rank-1 exactly once; Compiler checks input rank.
 * Layout, raw-bit, error and owner rules are the same as reshape_node.
 */
inline Result<WorkflowNode> transpose_node(
    std::uint64_t id, WorkflowInput input,
    const std::vector<std::uint64_t>& permutation,
    TransformLayout layout = TransformLayout::Auto,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return layout_detail::node(id, "transpose", {std::move(input)}, "permutation",
                             permutation, true, layout, profile);
}
/** @brief Authors a dynamic slice with static positive per-axis counts.
 * starts and steps must be Int64[rank]. For nonempty demand the operation
 * collects active inputs, then validates full slice endpoints using widened
 * integer arithmetic. Starts are absolute nonnegative indices; used steps are
 * nonzero. Counts of one ignore their step numerically; the whole step port is
 * excluded only when all counts are one. Otherwise all step entries are
 * collected and validated, so even an unused entry can cause an upstream
 * failure. Empty reads no controls or data. Invalid controls report
 * InvalidArgument/InvalidDomain and InvalidSlice with axis/values.
 * Layout/lifetime rules match reshape_node.
 */
inline Result<WorkflowNode> slice_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput starts,
    WorkflowInput steps, const std::vector<std::uint64_t>& counts,
    TransformLayout layout = TransformLayout::Auto,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return layout_detail::node(
      id, "slice", {std::move(input), std::move(starts), std::move(steps)},
      "counts", counts, false, layout, profile);
}
}  // namespace ps::numeric
