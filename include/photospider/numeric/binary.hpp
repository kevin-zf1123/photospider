#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"

namespace ps::numeric {
namespace binary_detail {
inline Result<WorkflowNode> node(std::uint64_t id, const char* name,
                                 WorkflowInput first, WorkflowInput second,
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
               "invalid binary id/profile",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  return Result<WorkflowNode>(
      WorkflowNode{id,
                   std::string("numeric.") + name + suffix,
                   {std::move(first), std::move(second)},
                   {}});
}
}  // namespace binary_detail
/** @brief Shared Result-backed contract for the binary node helpers.
 * Each input Result contains exactly one tensor member in slot 0 and may also
 * contain fields. Its schema ID and facets may vary. Recognized tensor facets
 * and spatial metadata still receive full-input typed validation. Numeric NaN
 * acceptance does not bypass typed constraints such as alpha validity.
 * Metadata specialization requires matching input dtypes and complete sample
 * shapes, rank 1..8, positive extents, and at most 2^40 samples. The output is
 * a Result on port `values`, using schema `photospider.tensor`, tensor key
 * `samples`, the full logical sample shape, and empty facets. There is no
 * implicit cast or broadcast.
 *
 * A nonempty Whole request needs complete input support with Data, Validation
 * and Descriptor roles (mask 13). The coordinator supplies authorized tensor
 * windows, preserving compatible signed and zero strides without requiring
 * packed input collection. The operation computes and publishes one complete
 * packed Result; the executor then projects requested coordinates. Empty
 * requests may use a metadata-only poll, but perform no sample reads or
 * arithmetic and return empty tensor coverage. Any input change invalidates
 * all observed output coordinates. Special numerical identities retain both
 * input obligations.
 *
 * Shape or dtype mismatch returns TypeMismatch/Schema. Integer overflow
 * anywhere fails the invocation with OperationFailed/ArithmeticOverflow/Run
 * and no Atom key.
 * IEEE nonfinite results succeed. Source, work, capacity and cancellation
 * errors retain their categories. The complete output and fixed scratch use
 * managed resources even for sparse requests. Published Results retain
 * immutable storage beyond context lifetime, and cache witnesses retain both
 * input Results and their exact sample bits.
 *
 * Strict arithmetic uses round-to-nearest-even with gradual underflow.
 * Arithmetic preserves the CPU worker's floating environment, and execution
 * leaves the caller's rounding mode and exception flags unchanged. Directed
 * pow/angle refinement is capped at 4096 bits and may return ResourceExhausted.
 * Accelerated arithmetic follows CpuNumericProfile's final FP32 error bound;
 * bounded binary64 SIMD handles admitted ordinary ranges, with strict fallback
 * for unresolved cases. Exact integer, special-value and selected algebraic
 * results retain their defined rules. Named profiles require their CPU target.
 * Invalid node IDs or profiles return InvalidArgument/InvalidDomain/Schema.
 * The helpers own their node metadata and are pure and safe for concurrent use;
 * allocation may throw std::bad_alloc. The numeric_workflow example contains
 * public Result workflows and oracles.
 */
/** @brief Exact a+b; UInt8/Int64/Float32/64, checked integer range.
 * @note Uses the shared binary execution/ownership/error contract above.
 */
inline Result<WorkflowNode> add_node(
    std::uint64_t id, WorkflowInput a, WorkflowInput b,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return binary_detail::node(id, "add", std::move(a), std::move(b), profile);
}
/** @brief Exact a-b; UInt8/Int64/Float32/64, checked integer range.
 * @note Uses the shared binary execution/ownership/error contract above.
 */
inline Result<WorkflowNode> subtract_node(
    std::uint64_t id, WorkflowInput a, WorkflowInput b,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return binary_detail::node(id, "subtract", std::move(a), std::move(b),
                             profile);
}
/** @brief Exact a*b; UInt8/Int64/Float32/64, checked integer range.
 * @note Uses the shared binary execution/ownership/error contract above.
 */
inline Result<WorkflowNode> multiply_node(
    std::uint64_t id, WorkflowInput a, WorkflowInput b,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return binary_detail::node(id, "multiply", std::move(a), std::move(b),
                             profile);
}
/** @brief Float32/64 a/b; signed zero and infinity follow NUM-05D.
 * @note Uses the shared binary execution/ownership/error contract above.
 */
inline Result<WorkflowNode> divide_node(
    std::uint64_t id, WorkflowInput a, WorkflowInput b,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return binary_detail::node(id, "divide", std::move(a), std::move(b), profile);
}
/** @brief All four dtypes; propagates first NaN, mixed zeros select -0.
 * @note Uses the shared binary execution/ownership/error contract above.
 */
inline Result<WorkflowNode> minimum_node(
    std::uint64_t id, WorkflowInput a, WorkflowInput b,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return binary_detail::node(id, "minimum", std::move(a), std::move(b),
                             profile);
}
/** @brief All four dtypes; propagates first NaN, mixed zeros select +0.
 * @note Uses the shared binary execution/ownership/error contract above.
 */
inline Result<WorkflowNode> maximum_node(
    std::uint64_t id, WorkflowInput a, WorkflowInput b,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return binary_detail::node(id, "maximum", std::move(a), std::move(b),
                             profile);
}
/** @brief Float32/64 real a^b; NaN^0 and 1^NaN give +1 after both reads.
 * @note Uses the shared binary execution/ownership/error contract above.
 */
inline Result<WorkflowNode> pow_node(
    std::uint64_t id, WorkflowInput a, WorkflowInput b,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return binary_detail::node(id, "pow", std::move(a), std::move(b), profile);
}
/** @brief Float32/64 oriented angle atan2(y,x), radians in [-pi,pi].
 * @note Uses the shared binary execution/ownership/error contract above.
 */
inline Result<WorkflowNode> atan2_node(
    std::uint64_t id, WorkflowInput y, WorkflowInput x,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return binary_detail::node(id, "atan2", std::move(y), std::move(x), profile);
}
/** @brief Float32/64 atan2(y,x)/exact pi; no rounded radian intermediate.
 * @note Uses the shared binary execution/ownership/error contract above.
 */
inline Result<WorkflowNode> atan2pi_node(
    std::uint64_t id, WorkflowInput y, WorkflowInput x,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return binary_detail::node(id, "atan2pi", std::move(y), std::move(x),
                             profile);
}
}  // namespace ps::numeric
