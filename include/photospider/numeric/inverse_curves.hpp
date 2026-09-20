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
 * fail TypeMismatch. Every nonempty request validates all x/y before local
 * query samples, even on exact knot/clamp paths. Typed/upstream closures remain
 * dependencies. Empty reads no payload. Output owns packed regions with correct
 * global origins beyond context teardown; arbitrary legal source strides apply.
 *
 * Strict rounds the complete rational expression once. Accelerated follows
 * CpuNumericProfile's FP32 bound and retains the monotone inverse mapping.
 * Knot/clamp conversion preserves x's zero sign; other exact zero is +0 and
 * nonzero underflow preserves sign. Invalid topology/nonfinite query/rejected
 * domain fails OperationFailed/InvalidDomain at the dependent Atom. Only the
 * selected final x may fail ArithmeticOverflow; unreturned endpoints cannot.
 * Work, capacity, cancellation, stale and upstream failures retain categories.
 * Full x/y witnesses invalidate all dependent outputs; query witnesses are
 * local. No output allocation proportional to unrequested N is required.
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
 * unique destination rounding; Float64 and unresolved cases use a reported
 * strict solver. Exact collinear stencils reduce to linear inversion, while
 * knot/clamp conversions remain exact. The combined mapping follows the same
 * monotone inverse independently of request order/partition. Host
 * work/cancellation checks cover every root comparison and limb multiplication;
 * no approximate result replaces exhaustion.
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
