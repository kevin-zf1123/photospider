#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"

namespace ps::numeric {
/** @brief Authors a Whole Result operation for y=M*x+b.
 * The `vectors`, `matrix`, and `bias` input edges each reference one Result
 * tensor member under any member key. Their sample shapes are [...,Cin],
 * [Cout,Cin], and [Cout]; all dtypes match and are Float32 or Float64, Cin/Cout
 * are in 2..4, and vector rank is 1..8. Input and output logical counts are at
 * most 2^40. The `values` output is a Result with `photospider.tensor` schema,
 * tensor member `samples`, shape [...,Cout], no facets, and no batch topology.
 * The returned node has no parameters, casts, implicit broadcast, inverse, or
 * homogeneous-coordinate division; singular matrices are valid.
 *
 * Nonempty demand validates Data, Validation, and Descriptor for every full
 * input and computes a complete dense output. Sparse and partial consumers
 * project that output; a change to any input dirties every observed output.
 * Empty demand publishes empty tensor coverage without matrix arithmetic.
 * Products and their sum with bias are exact before one RN-even conversion.
 * Source NaN priority is vector components, selected matrix row, then bias.
 * A zero-times-infinity product or opposite infinities produce positive qNaN.
 * Finite exact zero is negative only when every product and bias are negative
 * zero; overflow to infinity succeeds.
 *
 * Shape and dtype failures return TypeMismatch/Schema. Source, typed
 * validation, budget, and cancellation failures retain their categories, and
 * failed work publishes no partial output. Full output storage and fixed
 * callback scratch must fit the host resource budget. Published Result storage
 * owns its lifetime beyond context retirement.
 *
 * @param id Nonzero workflow node identifier.
 * @param vectors Workflow reference for the vectors Result input.
 * @param matrix Workflow reference for the matrix Result input.
 * @param bias Workflow reference for the bias Result input.
 * @param profile Registered strict or accelerated CPU numeric profile.
 * @return A WorkflowNode with the three ordered Result input edges, or
 *   InvalidArgument/Schema when `id` is zero or `profile` is unsupported.
 *
 * This helper creates node metadata without mutating shared state and is safe
 * for concurrent use. Allocation failure may throw `std::bad_alloc`.
 */
inline Result<WorkflowNode> matrix_transform_node(
    std::uint64_t id, WorkflowInput vectors, WorkflowInput matrix,
    WorkflowInput bias, CpuNumericProfile profile = CpuNumericProfile::Strict) {
  const auto* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!id || !suffix)
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               "invalid matrix id/profile",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  return Result<WorkflowNode>(
      WorkflowNode{id,
                   std::string("numeric.matrix_transform") + suffix,
                   {std::move(vectors), std::move(matrix), std::move(bias)},
                   {}});
}
}  // namespace ps::numeric
