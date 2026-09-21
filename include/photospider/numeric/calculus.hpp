#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"

namespace ps::numeric {
namespace calculus_detail {
inline Result<WorkflowNode> node(std::uint64_t id, const char* operation,
                                 std::vector<WorkflowInput> inputs,
                                 CpuNumericProfile profile) {
  const auto* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!id || !suffix)
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               "invalid calculus id/profile",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  return Result<WorkflowNode>(
      WorkflowNode{id,
                   std::string("numeric.") + operation + suffix,
                   std::move(inputs),
                   {}});
}
}  // namespace calculus_detail
/** @brief Authors exact discrete differences with one RN-even conversion.
 * samples [N], 2<=N<=2^40, and step [1] share Float32/64 dtype. Output values
 * [N] has empty facets. Endpoints use adjacent one-sided differences; interiors
 * use (samples[i+1]-samples[i-1])/(2*step). Whole reads/validates full samples
 * and step for nonempty demand, then computes complete dense output. Only the
 * two stencil values enter each numerical result. Source failure may precede
 * callback step validation. Zero/nonfinite step fails Domain/Run
 * InvalidArgument/InvalidDomain with InvalidSampleStep. Negative finite step is
 * valid. Source NaN priority is ascending source index; exact zero is +0.
 * Finite underflow/overflow and exceptional values follow the stated exact
 * formula. Any active input edit invalidates all recorded observations. Full
 * input collection, full output and fixed exact workspace are required even
 * for sparse demand. Empty requests read nothing. Shape/dtype errors are
 * TypeMismatch/Schema; upstream, typed, cancellation and resource failures
 * retain their categories. Helpers are pure/concurrent-safe, return owned
 * metadata, may throw bad_alloc, and reject invalid id/profile with
 * InvalidArgument/Schema. Published storage survives context retirement. This
 * discrete rule approximates a continuous derivative; it does not infer one
 * from an unknown underlying function.
 */
inline Result<WorkflowNode> derivative_1d_node(
    std::uint64_t id, WorkflowInput samples, WorkflowInput step,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return calculus_detail::node(id, "derivative_1d",
                               {std::move(samples), std::move(step)}, profile);
}
/** @brief Authors initial + step/2 times an exact weighted sample prefix.
 * samples [N], 1<=N<=2^40, step [1] and initial [1] share Float32/64 dtype.
 * values [N] retains empty facets. Output zero copies initial bits, including
 * sNaN and signed zero. Only N=1 excludes samples/step via static projection;
 * for N>1 all inputs are read/validated even for output0 projection. At
 * positive i, weights on samples[0..i] are 1,2,...,2,1 and the whole expression
 * rounds once. NaN priority is initial then ascending source indices; exact
 * zero is +0. Whole preparation precedes step validation; invalid step or any
 * upstream failure affects the Run. Exact prefix state survives final
 * conversion and complete output is published before projection. Other errors,
 * ownership and authoring rules match derivative_1d_node. No regional
 * windows/checkpoints or per-output association state remains. The discrete
 * formula approximates an underlying integral, with exact rounding only for the
 * stated formula.
 */
inline Result<WorkflowNode> integrate_1d_node(
    std::uint64_t id, WorkflowInput samples, WorkflowInput step,
    WorkflowInput initial,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return calculus_detail::node(
      id, "integrate_1d",
      {std::move(samples), std::move(step), std::move(initial)}, profile);
}
}  // namespace ps::numeric
