#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/data/tensor_span.hpp"
#include "photospider/ops/numeric/diagnostics.hpp"

namespace ps::numeric {
namespace ordering_detail {
inline Result<WorkflowNode> invalid(const char* message) {
  return Result<WorkflowNode>(
      Status{ErrorCode::InvalidArgument,
             message,
             FailureReason::InvalidDomain,
             {FailureOrigin::Schema, FailureScope::Unspecified}});
}
inline Result<WorkflowNode> node(std::uint64_t id, const char* operation,
                                 WorkflowInput input, std::int64_t axis,
                                 CpuNumericProfile profile) {
  const auto* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!id || axis < 0 || axis >= 8 || !suffix)
    return invalid("invalid ordering id/axis/profile");
  return Result<WorkflowNode>(WorkflowNode{id,
                                           std::string(operation) + suffix,
                                           {std::move(input)},
                                           {{"axis", axis}}});
}
}  // namespace ordering_detail
/** @brief Authors stable ascending per-axis sort with values and indices.
 * Source UInt8/Int64/Float32/64 has complete sample rank 1..8, positive extents
 * and at most 2^40 samples. Both ordinary outputs retain the full shape;
 * values preserves source bits, indices is Int64 with original axis positions.
 * NaNs sort last, signed zeros compare equal and ties retain source order.
 * Selected tiles with identical non-axis regions share one fixed CPU task,
 * including tiles from both output ports. Each task reads the complete axis
 * lines intersecting that region and sorts each line once. It writes only
 * declared output tiles, with independent dense allocations. Empty selections
 * read no payload. Preallocated per-invocation scratch contains 16*L bytes for
 * keys/permutation and fixed comparison state. Preparation checks schemas,
 * canonical writes, complete reads and scratch; runtime checks cancellation
 * during classification, comparisons and output writes. Task failure prevents
 * successful publication of the call. Immutable outputs can outlive context.
 * Helpers return owned metadata and permit concurrent calls. Invalid helper
 * arguments return InvalidArgument/InvalidDomain/Schema; allocation can throw
 * bad_alloc. Compilation checks the actual type/rank/axis. Operational source,
 * resource and cancellation failures retain their status categories.
 */
inline Result<WorkflowNode> sort_node(
    std::uint64_t id, WorkflowInput input, std::int64_t axis,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return ordering_detail::node(id, "array.sort", std::move(input), axis,
                               profile);
}
/** @brief Authors one exact quantile per selected source axis line.
 * The output keeps rank with axis extent 1 and Float32/64 dtype (Float64 by
 * default). The independent q tensor must have complete sample shape [1]
 * and Float32/64 dtype. When L>1 the caller supplies finite q in [0,1]; this
 * numerical precondition has no guaranteed out-of-domain diagnostic. L=1
 * checks q schema but reads no q payload or producer. Each selected output
 * tile reads its complete source lines and, when active, the q scalar.
 * Stable ordering and exact h=(L-1)*q precede exact interpolation and one final
 * RN conversion; integers never convert prematurely. The first NaN in source
 * axis order wins even at endpoint q, quieted with the reduction payload rule.
 * All IEEE source values are valid; finite Float64-to-Float32 conversion may
 * overflow to signed infinity. Scratch adds fixed exact arithmetic state to
 * sort_node's 16*L bytes. It is reused across lines within one invocation.
 * Ownership, concurrency, static validation and operational failure rules
 * follow sort_node. Empty selections emit no task or payload dependency.
 */
inline Result<WorkflowNode> quantile_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput q, std::int64_t axis,
    ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  if (dtype != ElementType::Float32 && dtype != ElementType::Float64)
    return ordering_detail::invalid("quantile requires float destination");
  auto result = ordering_detail::node(id, "numeric.quantile", std::move(input),
                                      axis, profile);
  if (!result.ok())
    return result;
  auto node = result.take_value();
  node.inputs.push_back(std::move(q));
  node.parameters["dtype"] =
      std::string(dtype == ElementType::Float32 ? "float32" : "float64");
  return Result<WorkflowNode>(std::move(node));
}
}  // namespace ps::numeric
