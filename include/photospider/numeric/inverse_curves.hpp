#pragma once

#include <cstdint>
#include <utility>

#include "photospider/numeric/curves.hpp"

namespace ps::numeric {
namespace inverse_curve_detail {
inline Result<WorkflowNode> node(std::uint64_t id, bool pchip, WorkflowInput x,
                                 WorkflowInput y, WorkflowInput query,
                                 ElementType dtype, CurveDomain policy,
                                 CpuNumericProfile profile) {
  if (policy != CurveDomain::Reject && policy != CurveDomain::Clamp)
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               "inverse curves require reject or clamp",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  return curve_detail::node(id, pchip ? "invert_pchip" : "invert_linear",
                            std::move(x), std::move(y), std::move(query), dtype,
                            policy, profile);
}
}  // namespace inverse_curve_detail
/** @brief Inverts an exact piecewise-linear function before output rounding.
 * x[K], y[K], query[N] independently accept Float32/64; K=2..65536 and
 * N=1..2^40. x must be finite/strictly increasing; y finite/strictly monotone
 * in either direction. Output values[N] is generic with the selected dtype
 * (default Float64). Policy is Reject by default or explicit Clamp.
 *
 * Construction is pure/thread-safe and may throw bad_alloc. Invalid id/profile/
 * policy/dtype fails InvalidArgument/InvalidDomain/Schema; incompatible ports
 * fail TypeMismatch. Every nonempty request collects complete x/y/query,
 * validates all topology and query controls, then computes dense values[N].
 * Empty reads no payload. Legal source strides and offsets are supported;
 * output ownership survives context teardown. Complete inputs/output and
 * promoted 16*K element bytes plus overhead consume managed capacity/work.
 *
 * Strict rounds the complete expression once. Accelerated follows the shared
 * FP32 bound and retains the monotone inverse mapping. Knot/clamp conversion
 * preserves x's zero sign; other exact zero is +0, underflow preserves sign.
 * Dynamic topology/query errors use OperationFailed/InvalidDomain/Domain/Run;
 * final x overflow uses ArithmeticOverflow. Undelivered positions can fail.
 * Unused endpoint narrowing does not reject a finite root. Any input edit
 * invalidates the complete output. Cancellation and resource/upstream failures
 * retain categories. No partial output is published on failure.

 */
inline Result<WorkflowNode> invert_linear_node(
    std::uint64_t id, WorkflowInput x, WorkflowInput y, WorkflowInput query,
    ElementType dtype = ElementType::Float64,
    CurveDomain policy = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return inverse_curve_detail::node(id, false, std::move(x), std::move(y),
                                    std::move(query), dtype, policy, profile);
}
/** @brief Inverts the original exact PCHIP polynomial with one final rounding.
 * Shares invert_linear_node's interface, global validation, errors and owners.
 * It does not swap x/y and fit a new curve. Exact destination lattice and
 * midpoint sign tests include zero-derivative, subnormal and overflow
 * boundaries. Accelerated Float32 uses certified bracketed iteration with
 * unique destination rounding; Float64 and unresolved cases use the strict
 * solver. Whole numerical/fallback diagnostics are unavailable. Exact collinear
 * stencils reduce to linear inversion, while knot/clamp conversions remain
 * exact. The combined mapping follows the same monotone inverse independently
 * of request order/partition. Host work/cancellation checks cover every root
 * comparison and limb multiplication; no approximate result replaces
 * exhaustion.
 */
inline Result<WorkflowNode> invert_pchip_node(
    std::uint64_t id, WorkflowInput x, WorkflowInput y, WorkflowInput query,
    ElementType dtype = ElementType::Float64,
    CurveDomain policy = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return inverse_curve_detail::node(id, true, std::move(x), std::move(y),
                                    std::move(query), dtype, policy, profile);
}
}  // namespace ps::numeric
