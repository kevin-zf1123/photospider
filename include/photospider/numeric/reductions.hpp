#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"
#include "photospider/data/value.hpp"

namespace ps::numeric {
namespace reduction_detail {
inline Result<WorkflowNode> invalid(const char* message) {
  return Result<WorkflowNode>(
      Status{ErrorCode::InvalidArgument,
             message,
             FailureReason::InvalidDomain,
             {FailureOrigin::Schema, FailureScope::Unspecified}});
}
inline const char* dtype_name(ElementType type) {
  return type == ElementType::UInt8     ? "uint8"
         : type == ElementType::Int64   ? "int64"
         : type == ElementType::Float32 ? "float32"
         : type == ElementType::Float64 ? "float64"
                                        : nullptr;
}
inline Result<WorkflowNode> node(std::uint64_t id, const char* operation,
                                 WorkflowInput input,
                                 std::vector<std::uint64_t> axes,
                                 CpuNumericProfile profile) {
  const auto* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!id || axes.empty() || axes.size() > 8 || !suffix)
    return invalid("invalid reduction id/axes/profile");
  std::sort(axes.begin(), axes.end());
  std::string encoded;
  for (std::size_t j = 0; j < axes.size(); ++j) {
    if (axes[j] >= 8 || (j && axes[j] == axes[j - 1]))
      return invalid("reduction axes must be distinct in 0..7");
    if (j)
      encoded += ',';
    encoded += std::to_string(axes[j]);
  }
  return Result<WorkflowNode>(
      WorkflowNode{id,
                   std::string("numeric.") + operation + suffix,
                   {std::move(input)},
                   {{"axes", std::move(encoded)}}});
}
inline Result<WorkflowNode> with_dtype(Result<WorkflowNode> result,
                                       ElementType dtype) {
  const auto* name = dtype_name(dtype);
  if (!name)
    return invalid("unsupported reduction dtype");
  if (!result.ok())
    return result;
  auto node = result.take_value();
  node.parameters["dtype"] = std::string(name);
  return Result<WorkflowNode>(std::move(node));
}
}  // namespace reduction_detail
/** @brief Authors exact sum over nonempty axes with fixed keepdims=true.
 * Source rank is 1..8 and logical count <=2^40. Reduced extents become one;
 * output values has empty facets. input_type is an authoring hint for the
 * default destination (Int64 for integers, Float64 for floats), not a runtime
 * parameter. The compiler validates actual source/destination domains. Explicit
 * output_type may select UInt8/Int64 for integer inputs or Float32/64 for
 * floats. Source terms sum exactly with only final range checking or IEEE
 * RN-even. Whole reads/validates every input group for nonempty demand and
 * computes complete dense output before projection; Empty reads nothing.
 * Any source edit invalidates all output observations. All helpers are
 * pure/concurrent-safe and return owned node metadata. Invalid authoring
 * arguments return InvalidArgument/InvalidDomain/Schema; allocation may throw
 * bad_alloc. Runtime source/resource/cancellation errors are preserved; integer
 * final overflow is Domain/Run OperationFailed/ArithmeticOverflow at its
 * output, including an unrequested group. Whole may own a full packed input
 * plus complete output and fixed exact state. Results retain immutable owned
 * backing after the execution context retires.
 */
inline Result<WorkflowNode> reduce_sum_node(
    std::uint64_t id, WorkflowInput input, std::vector<std::uint64_t> axes,
    ElementType input_type, std::optional<ElementType> output_type = {},
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  if (!reduction_detail::dtype_name(input_type))
    return reduction_detail::invalid("unsupported input dtype hint");
  const auto destination = output_type.value_or(
      input_type == ElementType::Float32 || input_type == ElementType::Float64
          ? ElementType::Float64
          : ElementType::Int64);
  return reduction_detail::with_dtype(
      reduction_detail::node(id, "reduce_sum", std::move(input),
                             std::move(axes), profile),
      destination);
}
/** @brief Same-dtype numerical minimum; first row-major NaN is quieted.
 * Mixed zeros select -0. Shape/dependency/error/lifetime rules match sum.
 */
inline Result<WorkflowNode> reduce_minimum_node(
    std::uint64_t id, WorkflowInput input, std::vector<std::uint64_t> axes,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return reduction_detail::node(id, "reduce_minimum", std::move(input),
                                std::move(axes), profile);
}
/** @brief Same-dtype maximum; mixed zeros select +0, first NaN wins. */
inline Result<WorkflowNode> reduce_maximum_node(
    std::uint64_t id, WorkflowInput input, std::vector<std::uint64_t> axes,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return reduction_detail::node(id, "reduce_maximum", std::move(input),
                                std::move(axes), profile);
}
/** @brief Exact sum/count rounded once to Float32/64, default Float64.
 * Integer source terms are never converted to floating point before summing.
 */
inline Result<WorkflowNode> reduce_mean_node(
    std::uint64_t id, WorkflowInput input, std::vector<std::uint64_t> axes,
    ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  if (dtype != ElementType::Float32 && dtype != ElementType::Float64)
    return reduction_detail::invalid("mean requires a float destination");
  return reduction_detail::with_dtype(
      reduction_detail::node(id, "reduce_mean", std::move(input),
                             std::move(axes), profile),
      dtype);
}
/** @brief Metadata-only exact Int64 group count; no numeric input is read.
 * Whole excludes the runtime input port and owns one8-byte zero-stride complete
 * output. Input-byte changes do not
 * invalidate counts; shape and axes remain descriptor dependencies.
 */
inline Result<WorkflowNode> reduce_count_node(
    std::uint64_t id, WorkflowInput input, std::vector<std::uint64_t> axes,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return reduction_detail::node(id, "reduce_count", std::move(input),
                                std::move(axes), profile);
}
namespace reduction_detail {
inline Result<WorkflowNode> moments(std::uint64_t id, const char* operation,
                                    WorkflowInput input,
                                    std::vector<std::uint64_t> axes,
                                    std::int64_t ddof, ElementType dtype,
                                    CpuNumericProfile profile) {
  if (ddof < 0 ||
      (dtype != ElementType::Float32 && dtype != ElementType::Float64))
    return invalid("invalid moments ddof/dtype");
  auto result = with_dtype(
      node(id, operation, std::move(input), std::move(axes), profile), dtype);
  if (!result.ok())
    return result;
  auto authored = result.take_value();
  authored.parameters["ddof"] = ddof;
  return Result<WorkflowNode>(std::move(authored));
}
}  // namespace reduction_detail
/** @brief RN of exact (N*sum(x*x)-sum(x)^2)/(N*(N-ddof)).
 * ddof defaults to zero and must be below static group count. Invalid ddof
 * fails compilation/preflight before sample reads. NaN priority uses logical
 * row-major order; any infinity without NaN produces the fixed positive qNaN.
 */
inline Result<WorkflowNode> reduce_variance_node(
    std::uint64_t id, WorkflowInput input, std::vector<std::uint64_t> axes,
    std::int64_t ddof = 0, ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return reduction_detail::moments(id, "reduce_variance", std::move(input),
                                   std::move(axes), ddof, dtype, profile);
}
/** @brief RN of the exact variance's square root, without rounding variance.
 * Uses the same ports, keepdims, ddof/dtype and nonfinite rules as variance.
 */
inline Result<WorkflowNode> reduce_std_node(
    std::uint64_t id, WorkflowInput input, std::vector<std::uint64_t> axes,
    std::int64_t ddof = 0, ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return reduction_detail::moments(id, "reduce_std", std::move(input),
                                   std::move(axes), ddof, dtype, profile);
}
}  // namespace ps::numeric
