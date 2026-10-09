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
/** @brief Authors a row-major logical reshape with an explicit target shape.
 * The source and result are single-tensor Result objects. The source tensor may
 * use any schema id/member key; its complete sample_shape() has rank 1..8,
 * positive extents and at most 2^40 elements. UInt8, Int8, UInt16, Int16,
 * Int64, Float32 and Float64 are supported; the target keeps the source dtype
 * and preserves every element bit, including signaling-NaN payloads.
 * The published photospider.tensor/samples Result has the complete target shape
 * as ordinary axes, with facets and batch-axis metadata dropped.
 *
 * This pure, concurrent-safe helper only authors node metadata and reads no
 * payload. Static schema and parameter checks run before the CPU Whole program
 * requests active tensors through Tensor Needs. A nonempty request uses role
 * 13 to trigger typed-payload validation, then publishes a complete Result
 * with global output coordinates. A downstream query limits observed
 * dependencies; it does not turn the publication into a packed ROI result.
 * View requires one proven affine source/output owner. If the complete mapping
 * is unavailable, View reports Domain/Run InvalidArgument with diagnostic
 * ViewUnavailable. Compatible fragments may join when they share that owner.
 * Auto materializes a complete packed output only when a view is unavailable;
 * Dense always materializes it and owns N*dtype_size bytes, including for a
 * sparse downstream query. Resource, validation and cancellation errors remain
 * errors rather than Auto fallbacks.
 * Published views retain their source storage and resources after context
 * destruction. Layout registrations disable cross-run content caching because
 * content alone does not identify physical owners or strides.
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
 * starts and steps are Int64[rank] Result inputs. For nonempty demand the
 * operation requests each active input through a full Tensor Need with Data,
 * Validation and Descriptor roles (role 13), triggering typed-payload
 * validation. It checks full slice endpoints using widened integer arithmetic.
 * Starts are absolute
 * nonnegative indices; used steps are nonzero. Counts of one ignore their step
 * numerically; the whole step port is
 * excluded from runtime Need and association only when all counts are one;
 * static schema and parameter checks still apply. Otherwise all step entries
 * are requested and validated, so even an unused entry can cause an upstream
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
