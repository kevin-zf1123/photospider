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
/** @brief Shared contract of the independently named binary node helpers.
 * Inputs must match dtype and positive rank-1..8 shape, with count <=2^40;
 * output values preserves both with empty facets. There is no implicit cast or
 * broadcast. Both sources retain exact requested Data and typed Validation,
 * including special results such as NaN^0. Shape/dtype errors fail
 * TypeMismatch/Schema; integer overflow is OperationFailed/ArithmeticOverflow
 * for the requested Atom. IEEE nonfinite results succeed. Source, work,
 * capacity and cancellation failures retain their categories.
 * Helpers own metadata and are pure/concurrent-safe; allocation may throw
 * bad_alloc. Invalid id/profile fails InvalidArgument/InvalidDomain/Schema.
 * Runtime results own immutable packed storage beyond context life. Cache
 * witnesses retain both inputs and their exact bits, including NaN payloads.
 * Arithmetic is RN-even with gradual underflow and unchanged caller fenv.
 * Ordinary pow/angle values use directed refinement through 4096 fractional
 * bits; unresolved rounding fails ResourceExhausted. Accelerated profiles
 * report FunctionUnsupported strict fallback for ordinary transcendental
 * values; special/algebraic cases are exact. Named profiles require their CPU.
 * See examples/numeric_workflow for editable workflows and explicit math
 * budgets.
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
