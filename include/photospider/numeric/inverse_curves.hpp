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
 * Each input is a Result with one tensor member under any schema id/member key;
 * sample_shape() supplies x[K], y[K], query[N]. Inputs independently accept
 * Float32/64, K=2..65536, N=1..2^40. x must be finite and strictly
 * increasing; y must be finite and strictly monotone in either direction.
 * The `values` Result uses schema photospider.tensor/member samples with shape
 * [N], empty facets and selected dtype (default Float64). Policy defaults to
 * Reject; Clamp is also supported. Compiler checks bound input metadata; this
 * helper owns node metadata and may be used concurrently. Construction may
 * throw bad_alloc. Invalid id/profile/policy/dtype fails
 * InvalidArgument/InvalidDomain/Schema; incompatible edge metadata fails
 * TypeMismatch.
 *
 * Every nonempty Whole request reads all inputs with Data, Validation and
 * Descriptor (role 13), validates global topology and all query controls, then
 * publishes the complete dense output with source ObjectId association. Empty
 * reads no payload. Legal source strides/offsets are supported; output
 * ownership survives context teardown. Promoted inputs use 16*K element bytes
 * plus overhead, and complete input/output storage consumes managed
 * capacity/work.
 *
 * Strict rounds the complete expression once. Accelerated follows the shared
 * FP32 bound and retains the monotone inverse mapping. Knot/clamp conversion
 * preserves x's zero sign; other exact zero is +0 and underflow preserves sign.
 * Dynamic topology/query errors use OperationFailed/InvalidDomain/Domain/Run;
 * final x overflow uses ArithmeticOverflow. Undelivered positions can fail.
 * Unused endpoint narrowing does not reject a finite root. Any input edit
 * invalidates complete output. Cancellation/resource/upstream failures retain
 * categories; failure publishes no partial Result.
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
