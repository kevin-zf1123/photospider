#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"

namespace ps::numeric {
/** @brief Authors y=M*x+b with exact products/sum and one RN-even conversion.
 * Inputs share Float32/64 dtype: vectors [...,Cin], matrix [Cout,Cin], bias
 * [Cout], with Cin/Cout in 2..4 and vector rank 1..8. Input/output counts are
 * <=2^40. Output values [...,Cout] has empty facets. There are no parameters,
 * casts or implicit broadcast; singular matrices are valid. Nonempty requests
 * use Whole execution: validate all inputs and compute the complete output.
 * Partial consumers project that result; any input change invalidates all
 * observed outputs. Empty requests skip computation.
 * Source NaN priority is vector components, selected matrix row, then bias.
 * Otherwise zero*infinity or opposite infinities produce positive qNaN. Finite
 * exact zero is negative only when every product and bias is negative zero.
 * Shape/dtype failures are TypeMismatch/Schema; source, validation, budget and
 * cancellation errors retain their categories. Overflow to infinity succeeds.
 * The helper is pure/concurrent-safe, returns owned node metadata, may throw
 * bad_alloc, and rejects invalid id/profile with InvalidArgument/Schema.
 * Published runtime storage survives context retirement. Full output storage
 * and fixed callback scratch must fit the host resource budget.
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
