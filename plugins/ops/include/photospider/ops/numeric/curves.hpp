#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"
#include "photospider/data/value.hpp"

namespace ps::numeric {
/** @brief Explicit policy for finite queries outside the knot interval. */
enum class CurveDomain { Reject, Clamp, LinearExtrapolate };
namespace curve_detail {
inline Result<WorkflowNode> node(std::uint64_t id, const char* operation,
                                 WorkflowInput x, WorkflowInput y,
                                 WorkflowInput query, ElementType dtype,
                                 CurveDomain policy,
                                 CpuNumericProfile profile) {
  const auto* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  const auto* domain = policy == CurveDomain::Reject  ? "reject"
                       : policy == CurveDomain::Clamp ? "clamp"
                       : policy == CurveDomain::LinearExtrapolate
                           ? "linear_extrapolate"
                           : nullptr;
  if (!id || !suffix || !domain ||
      (dtype != ElementType::Float32 && dtype != ElementType::Float64))
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               "invalid curve id/dtype/policy/profile",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  return Result<WorkflowNode>(WorkflowNode{
      id,
      std::string("curve.") + operation + suffix,
      {std::move(x), std::move(y), std::move(query)},
      {{"dtype",
        std::string(dtype == ElementType::Float32 ? "float32" : "float64")},
       {"out_of_domain", std::string(domain)}}});
}
}  // namespace curve_detail
/** @brief Shared contract for the four independent interpolation constructors.
 * Each input is a Result containing one tensor member under any schema id and
 * member key. Its sample_shape() is x[K], y[K] (single) or y[K,C] (multi),
 * and query[N]. Each input independently accepts Float32/64. K=2..65536 and
 * positive logical products are <=2^40. The output port `values` is a Result
 * with schema photospider.tensor, member `samples`, shape [N] or [N,C]
 * (including C=1), selected dtype and empty facets. Batch dimensions are
 * ordinary output axes. Compiler checks actual input edges; helpers own node
 * metadata and may be used concurrently. Nonempty Whole execution requests
 * Data, Validation and Descriptor (role 13) for all three inputs, computes all
 * queries and columns, and publishes a complete dense output. The Result
 * association names the actual source ObjectIds. Allocation may throw
 * bad_alloc. Invalid helper parameters fail
 * InvalidArgument/InvalidDomain/Schema; invalid edge metadata fails
 * TypeMismatch. It validates all x knots before y arithmetic. Exact knot/clamp
 * uses one y; linear uses two endpoints; PCHIP uses its fixed local stencil.
 * Generic y values outside evaluated stencils are numerically unused. Nonfinite
 * used input/rejected query fails OperationFailed/InvalidDomain at Run scope;
 * final overflow fails ArithmeticOverflow. Unrequested rows/columns and
 * upstream failures can fail the run. Output owns immutable packed storage
 * beyond context lifetime. Typed, resource, stale and cancellation failures
 * retain their categories. Any input edit invalidates output demand. Empty
 * reads no payload. Strict rounds complete exact rational formulas once.
 * Accelerated follows CpuNumericProfile's bound while preserving cross-query
 * monotonicity; the current fast Float32 path requires a uniquely rounded
 * enclosure and Float64 uses exact formulas. Caller fenv is preserved.
 * Ordinary exact zero is -0 only for two -0 segment endpoints; node/clamp
 * retains y zero. CPU-specific profiles require their target. PCHIP
 * extrapolates its endpoint tangent; linear extrapolates its endpoint secant.
 * See the numeric_workflow README for current Result test coverage and
 * compositions.
 */
/** @brief Piecewise-linear single-function interpolation; shared contract
 * above. */
inline Result<WorkflowNode> interpolate_linear_node(
    std::uint64_t id, WorkflowInput x, WorkflowInput y, WorkflowInput query,
    ElementType dtype = ElementType::Float64,
    CurveDomain policy = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return curve_detail::node(id, "interpolate_linear", std::move(x),
                            std::move(y), std::move(query), dtype, policy,
                            profile);
}
/** @brief Exact PCHIP single-function interpolation; shared contract above. */
inline Result<WorkflowNode> interpolate_pchip_node(
    std::uint64_t id, WorkflowInput x, WorkflowInput y, WorkflowInput query,
    ElementType dtype = ElementType::Float64,
    CurveDomain policy = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return curve_detail::node(id, "interpolate_pchip", std::move(x), std::move(y),
                            std::move(query), dtype, policy, profile);
}
/** @brief Piecewise-linear multiple-function interpolation; shared contract
 * above.
 */
inline Result<WorkflowNode> interpolate_linear_multi_node(
    std::uint64_t id, WorkflowInput x, WorkflowInput y, WorkflowInput query,
    ElementType dtype = ElementType::Float64,
    CurveDomain policy = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return curve_detail::node(id, "interpolate_linear_multi", std::move(x),
                            std::move(y), std::move(query), dtype, policy,
                            profile);
}
/** @brief Exact PCHIP multiple-function interpolation; shared contract above.
 */
inline Result<WorkflowNode> interpolate_pchip_multi_node(
    std::uint64_t id, WorkflowInput x, WorkflowInput y, WorkflowInput query,
    ElementType dtype = ElementType::Float64,
    CurveDomain policy = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return curve_detail::node(id, "interpolate_pchip_multi", std::move(x),
                            std::move(y), std::move(query), dtype, policy,
                            profile);
}
}  // namespace ps::numeric
