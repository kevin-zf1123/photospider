#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"
#include "photospider/data/value.hpp"

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
 * Input UInt8/Int64/Float32/64 has rank 1..8, positive extents and at most
 * 2^40 logical values. Both generic outputs retain its shape; values retains
 * source dtype/bits, indices is Int64 with original axis positions. NaNs sort
 * last, signed zeros compare equal, all ties retain original axis order.
 * Requested sorted positions depend on complete source lines plus typed
 * Validation. The unrequested public output is not allocated. Helpers return
 * owned node metadata and may be called concurrently. Invalid helper arguments
 * return InvalidArgument/InvalidDomain/Schema; allocation may throw bad_alloc.
 * Compiler validates the actual axis/rank/type. Runtime resource/upstream/typed
 * and cancellation failures retain their categories; no partial failed output
 * is published. Returned immutable owners can outlive ExecutionContext.
 */
inline Result<WorkflowNode> sort_node(
    std::uint64_t id, WorkflowInput input, std::int64_t axis,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return ordering_detail::node(id, "array.sort", std::move(input), axis,
                               profile);
}
/** @brief Authors one exact quantile per source axis line, keeping its
 * extent 1. q is an independent Float32/64 [1] array. For N>=2 it is read first
 * as Control and must be finite in [0,1], else InvalidArgument/InvalidDomain
 * with InvalidQuantileProbability at the output observation. For N=1 it is
 * unread. Stable order, exact h=(N-1)*q and exact linear interpolation precede
 * one Float32/64 rounding (default Float64); integers never convert
 * prematurely. The first source-line NaN in original order wins and is quieted
 * with the reduction payload mapping. Source lines are fully read even for
 * endpoint q. values has empty facets. Ownership, helper errors, resource and
 * typed Validation rules follow sort_node.
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
