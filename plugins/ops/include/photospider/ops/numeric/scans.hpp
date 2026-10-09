#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "photospider/ops/numeric/reductions.hpp"

namespace ps::numeric {
/** @brief Authors exact boundary prefix sums on one static axis.
 * Input rank is 1..8; output extends axis by one. Input and output counts must
 * be <=2^40. Output values has empty facets. Boundary zero is +0. For a
 * nonempty demand, Whole requests complete input support with Data, Validation
 * and Descriptor roles, including a zero-boundary projection. It reads through
 * authorized Result windows and does not pack the complete input; Empty reads
 * nothing. Each boundary k>0 sums [0,k) exactly and converts once, using
 * reduce_sum's same-domain dtype, source NaN priority, infinity and zero rules.
 * input_type selects the default Int64/Float64 destination; the compiler checks
 * the actual source domain.
 * Invalid arguments fail with InvalidArgument / InvalidDomain / Schema;
 * incompatible shapes/domains fail TypeMismatch. Any complete-output integer
 * overflow is Domain/Run OperationFailed / ArithmeticOverflow, including
 * unrequested output positions. Typed Validation, upstream, resource and
 * cancellation failures retain their own categories. Helpers return owned
 * metadata, are pure/concurrent-safe and may throw bad_alloc. Runtime results
 * own immutable storage after context retirement. Whole computes and publishes
 * the complete dense output in global coordinates with fixed exact state, even
 * for sparse demand. Requested output footprints scope observed dependency
 * roots; consumers can read any coordinate in the returned Result's complete
 * published coverage. Any input edit invalidates recorded observations. No
 * persistent checkpoint is kept.
 */
inline Result<WorkflowNode> prefix_sum_node(
    std::uint64_t id, WorkflowInput input, std::uint64_t axis,
    ElementType input_type, std::optional<ElementType> output_type = {},
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  auto result = reduce_sum_node(id, std::move(input), {axis}, input_type,
                                output_type, profile);
  if (!result.ok())
    return result;
  auto node = result.take_value();
  node.operation.replace(8, 10, "prefix_sum");
  node.parameters.erase("axes");
  node.parameters["axis"] = static_cast<std::int64_t>(axis);
  return Result<WorkflowNode>(std::move(node));
}
/** @brief Authors exact anchored rectangular sums on two distinct axes.
 * axes is normalized to increasing order, rank is 2..8, and each selected
 * extent increases by one. Other coordinates are independent batches. Either
 * zero boundary returns +0 after Whole input preparation. The complete input
 * support is requested for every nonempty output demand, and the kernel reads
 * through authorized Result windows without packing the full input. The
 * complete output is published in global coordinates; requested footprints
 * scope observed dependency roots, and consumers can read any coordinate in the
 * published coverage. Exact row prefixes merge with compact exact column
 * carries before final rounding. Columns use metadata proportional to the
 * higher-numbered selected axis extent, reset per plane. NaN priority follows
 * original row-major order. Arithmetic, dtype, ownership, error and concurrency
 * rules match prefix_sum_node. Floating four-corner subtraction of rounded
 * results is not an exact rectangle oracle.
 */
inline Result<WorkflowNode> integral_image_node(
    std::uint64_t id, WorkflowInput input, std::vector<std::uint64_t> axes,
    ElementType input_type, std::optional<ElementType> output_type = {},
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  if (axes.size() != 2)
    return reduction_detail::invalid("integral image requires two axes");
  auto result = reduce_sum_node(id, std::move(input), std::move(axes),
                                input_type, output_type, profile);
  if (!result.ok())
    return result;
  auto node = result.take_value();
  node.operation.replace(8, 10, "integral_image");
  return Result<WorkflowNode>(std::move(node));
}
}  // namespace ps::numeric
