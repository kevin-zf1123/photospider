#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"
#include "photospider/data/value.hpp"

namespace ps::numeric {
namespace unary_detail {
inline Result<WorkflowNode> node(std::uint64_t id, const char* name,
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
               "invalid unary id/profile",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  return Result<WorkflowNode>(
      WorkflowNode{id,
                   std::string("numeric.") + name + suffix,
                   std::move(inputs),
                   {}});
}
inline Result<WorkflowNode> rational(std::uint64_t id, const char* name,
                                     WorkflowInput numerator,
                                     WorkflowInput denominator,
                                     ElementType dtype,
                                     CpuNumericProfile profile) {
  if (dtype != ElementType::Float32 && dtype != ElementType::Float64)
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               "rational pi output must be Float32/64",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  auto result =
      node(id, name, {std::move(numerator), std::move(denominator)}, profile);
  if (!result.ok())
    return result;
  auto authored = result.take_value();
  authored.parameters["dtype"] =
      std::string(dtype == ElementType::Float32 ? "float32" : "float64");
  return Result<WorkflowNode>(std::move(authored));
}
}  // namespace unary_detail
/** @brief Shared contract of independently named unary node constructors.
 * Every helper returns owned metadata and is pure/concurrent-safe; allocation
 * may throw bad_alloc. Invalid id/profile/dtype parameters fail InvalidArgument
 * /InvalidDomain/Schema. Runtime input rank is 1..8 with positive extents and
 * count <=2^40. Output values preserves shape with empty facets. Ordinary unary
 * operations preserve dtype; support is stated per helper. Data demand is the
 * exact requested set; recognized typed Validation is retained separately.
 * Shape/dtype errors fail TypeMismatch/Schema. Integer overflow and invalid
 * rational denominators are attributed to the requested Atom. IEEE nonfinite
 * results succeed; source, typed, work, capacity and cancellation failures keep
 * their categories. Results own immutable packed storage beyond context life.
 * Strict math is RN-even with gradual underflow and unchanged caller fenv.
 * Directed refinement has a 4096-bit ceiling and can fail ResourceExhausted.
 * Accelerated arithmetic follows CpuNumericProfile's final FP32 error bound;
 * bounded binary64 SIMD kernels handle admitted ordinary ranges, with reported
 * strict fallback for unresolved cases. Special/algebraic paths remain exact.
 * Named profiles require their CPU target.
 * The accompanying numeric_workflow example shows explicit math work/state
 * budgets, public execution, oracle commands and checkable expected results.
 */
/** @brief Absolute value; UInt8/Int64/Float32/64, Int64 minimum overflows.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> abs_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "abs", {std::move(input)}, profile);
}
/** @brief Negation; Int64/Float32/64, Int64 minimum overflows; UInt8 rejects.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> neg_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "neg", {std::move(input)}, profile);
}
/** @brief Float32/64 correctly rounded root; negative nonzero input gives qNaN.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> sqrt_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "sqrt", {std::move(input)}, profile);
}
/** @brief Float32/64 correctly rounded natural exponential.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> exp_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "exp", {std::move(input)}, profile);
}
/** @brief Float32/64 natural logarithm; signed zero gives -Inf, negatives qNaN.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> ln_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "ln", {std::move(input)}, profile);
}
/** @brief Float32/64 sine of the exact radian value, with no angle snapping.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> sin_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "sin", {std::move(input)}, profile);
}
/** @brief Float32/64 cosine of the exact radian value.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> cos_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "cos", {std::move(input)}, profile);
}
/** @brief Float32/64 tangent of the exact radian value; no snapped poles.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> tan_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "tan", {std::move(input)}, profile);
}
/** @brief All four dtypes; floating floor with input sign on zero results.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> floor_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "floor", {std::move(input)}, profile);
}
/** @brief All four dtypes; floating ceil with input sign on zero results.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> ceil_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "ceil", {std::move(input)}, profile);
}
/** @brief All four dtypes; nearest integral value, ties to even; zero sign
 * kept.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> round_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "round", {std::move(input)}, profile);
}
/** @brief All four dtypes; signed unit for nonzero, zeros/quiet NaNs preserved.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> sign_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "sign", {std::move(input)}, profile);
}
/** @brief Float32/64 exact reciprocal; signed zero/Inf exchange.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> reciprocal_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "reciprocal", {std::move(input)}, profile);
}
/** @brief Float32/64 sin(pi*x); exact integers/halves/quarters before
 * refinement.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> sinpi_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "sinpi", {std::move(input)}, profile);
}
/** @brief Float32/64 cos(pi*x); half-integers return +0.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> cospi_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "cospi", {std::move(input)}, profile);
}
/** @brief Float32/64 tan(pi*x); half-integer poles return canonical qNaN.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> tanpi_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "tanpi", {std::move(input)}, profile);
}
/** @brief Float32/64 whole sin(x)/x; zero gives +1, infinity gives +0.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> sinc_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "sinc", {std::move(input)}, profile);
}
/** @brief Float32/64 whole sin(pi*x)/(pi*x); nonzero integers give +0.
 * @note Uses the shared unary helper execution/ownership/error contract above.
 */
inline Result<WorkflowNode> sincpi_node(
    std::uint64_t id, WorkflowInput input,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::node(id, "sincpi", {std::move(input)}, profile);
}
/** @brief Authors sinpi of the exact Int64 numerator/denominator ratio.
 * Inputs have identical shape. Positive denominator is required even at zero
 * numerator; invalid denominator is InvalidArgument/InvalidDomain/Atom. Both
 * inputs retain Data/Validation dependencies. Output dtype is explicit/default
 * Float64. Exact common-angle and pole/zero rules are documented in NUM-04S..V;
 * the complete quotient is used for sincpi, including its unreduced magnitude.
 * Other execution/ownership/resource rules match the shared unary contract.
 */
inline Result<WorkflowNode> sinpi_rational_node(
    std::uint64_t id, WorkflowInput numerator, WorkflowInput denominator,
    ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::rational(id, "sinpi_rational", std::move(numerator),
                                std::move(denominator), dtype, profile);
}
/** @brief Authors cospi of the exact Int64 numerator/denominator ratio.
 * Inputs have identical shape. Positive denominator is required even at zero
 * numerator; invalid denominator is InvalidArgument/InvalidDomain/Atom. Both
 * inputs retain Data/Validation dependencies. Output dtype is explicit/default
 * Float64. Exact common-angle and pole/zero rules are documented in NUM-04S..V;
 * the complete quotient is used for sincpi, including its unreduced magnitude.
 * Other execution/ownership/resource rules match the shared unary contract.
 */
inline Result<WorkflowNode> cospi_rational_node(
    std::uint64_t id, WorkflowInput numerator, WorkflowInput denominator,
    ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::rational(id, "cospi_rational", std::move(numerator),
                                std::move(denominator), dtype, profile);
}
/** @brief Authors tanpi of the exact Int64 numerator/denominator ratio.
 * Inputs have identical shape. Positive denominator is required even at zero
 * numerator; invalid denominator is InvalidArgument/InvalidDomain/Atom. Both
 * inputs retain Data/Validation dependencies. Output dtype is explicit/default
 * Float64. Exact common-angle and pole/zero rules are documented in NUM-04S..V;
 * the complete quotient is used for sincpi, including its unreduced magnitude.
 * Other execution/ownership/resource rules match the shared unary contract.
 */
inline Result<WorkflowNode> tanpi_rational_node(
    std::uint64_t id, WorkflowInput numerator, WorkflowInput denominator,
    ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::rational(id, "tanpi_rational", std::move(numerator),
                                std::move(denominator), dtype, profile);
}
/** @brief Authors sincpi of the exact Int64 numerator/denominator ratio.
 * Inputs have identical shape. Positive denominator is required even at zero
 * numerator; invalid denominator is InvalidArgument/InvalidDomain/Atom. Both
 * inputs retain Data/Validation dependencies. Output dtype is explicit/default
 * Float64. Exact common-angle and pole/zero rules are documented in NUM-04S..V;
 * the complete quotient is used for sincpi, including its unreduced magnitude.
 * Other execution/ownership/resource rules match the shared unary contract.
 */
inline Result<WorkflowNode> sincpi_rational_node(
    std::uint64_t id, WorkflowInput numerator, WorkflowInput denominator,
    ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return unary_detail::rational(id, "sincpi_rational", std::move(numerator),
                                std::move(denominator), dtype, profile);
}
}  // namespace ps::numeric
