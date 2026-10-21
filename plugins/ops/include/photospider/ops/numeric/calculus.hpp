#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/ops/numeric/diagnostics.hpp"

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
/** @brief Authors discrete differences with one final numeric conversion.
 * samples [N], 2<=N<=2^40, and step [1] share Float32/64 dtype. Output values
 * [N] has ordinary axes. Endpoints use adjacent one-sided differences;
 * interiors use (samples[i+1]-samples[i-1])/(2*step). Each selected output
 * tile has one fixed CPU task whose source reads cover exactly the union of
 * lower/upper stencil images, rounded to source tiles. It also reads step.
 * Finite nonzero step is a caller numerical precondition; negative step is
 * valid and out-of-domain diagnostics are not guaranteed. Source NaN priority
 * is increasing stencil index; exact zero is +0 and final overflow is signed
 * infinity. Strict rounds the exact quotient once; accelerated profiles follow
 * their stated final error budget. A task borrows preallocated ExactCalculus
 * scratch and owns an independent dense output tile. Empty emits no tasks.
 * Prepare checks schemas, canonical writes, complete reads and scratch. Shape
 * or dtype incompatibility returns TypeMismatch. Resource, upstream and
 * cancellation failures retain their operational categories and prevent the
 * call's successful publication. Immutable outputs can outlive the context.
 * Helpers return owned metadata, support concurrent calls, may throw bad_alloc,
 * and reject invalid id/profile with InvalidArgument/InvalidDomain/Schema.
 * The formula approximates a continuous derivative of sampled data.
 */
inline Result<WorkflowNode> derivative_1d_node(
    std::uint64_t id, WorkflowInput samples, WorkflowInput step,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return calculus_detail::node(id, "derivative_1d",
                               {std::move(samples), std::move(step)}, profile);
}
/** @brief Authors initial plus an exact weighted trapezoidal prefix.
 * samples [N], 1<=N<=2^40, step [1] and initial [1] share Float32/64 dtype.
 * values [N] has ordinary axes. Output zero copies initial bits, including
 * sNaN and signed zero. A canonical tile containing only zero has a separate
 * initial-only task with zero scratch and schema-only samples/step references.
 * This applies for N=1 and tile extent one. All other selected tiles share one
 * task reading samples through the greatest selected index, step and initial.
 * At positive i, weights on samples[0..i] are 1,2,...,2,1. Exact prefix state
 * survives every final conversion; unselected coordinates are not converted.
 * NaN priority is initial then increasing source index; exact zero is +0.
 * A saved Plan retains all writes and reads of a retained multiwrite task.
 * Task failures share the IR call's publication outcome, including initial-only
 * and prefix tasks. Each invocation owns scratch; concurrent Runs share only
 * immutable metadata. Other numerical preconditions, errors and ownership
 * rules match derivative_1d_node. This is a discrete trapezoidal approximation.
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
