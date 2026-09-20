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
 * x[K], y[K] (single) or y[K,C] (multi), query[N] independently accept
 * Float32/64. K=2..65536; positive logical products <=2^40. Output values has
 * shape [N] or [N,C] (including C=1), selected dtype and empty facets. Compiler
 * checks actual edges; helpers own metadata and may be used concurrently.
 * Allocation may throw bad_alloc. Invalid helper parameters fail
 * InvalidArgument/InvalidDomain/Schema; invalid edge metadata fails
 * TypeMismatch. Nonempty requests validate all x as finite/strictly increasing,
 * then only selected queries and exact local y stencils. Knot/clamp selects one
 * y; linear selects two, PCHIP up to four. Multi requests preserve column-local
 * support. Actual nonfinite input/rejected query fails OperationFailed/
 * InvalidDomain at the dependent Atom; final overflow fails ArithmeticOverflow.
 * Outputs own immutable packed storage beyond context lifetime. Typed,
 * upstream, resource, stale and cancellation failures retain their categories.
 * Cache witnesses include complete x, selected query and selected y. Empty
 * reads none. Strict rounds complete exact rational formulas once. Accelerated
 * follows CpuNumericProfile's bound while preserving cross-query monotonicity;
 * the current fast Float32 path requires a uniquely rounded enclosure and
 * Float64 uses exact formulas. Caller fenv is preserved. Ordinary exact zero is
 * -0 only for two -0 segment endpoints; node/clamp retains y zero. CPU-specific
 * profiles require their target. PCHIP extrapolates its endpoint tangent;
 * linear extrapolates its endpoint secant. See numeric_workflow examples for
 * explicit resource budgets and executable composition.
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
/** @brief Piecewise-linear column-local interpolation; shared contract above.
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
/** @brief Exact PCHIP column-local interpolation; shared contract above. */
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
