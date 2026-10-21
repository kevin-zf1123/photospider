#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "photospider/ops/numeric/reductions.hpp"

namespace ps::numeric {
/** @brief Authors exact boundary prefix sums on one static axis.
 * Source rank is 1..8; output extends axis by one, with both logical counts
 * <=2^40. Output values uses ordinary axes. input_type selects the default
 * Int64/Float64 destination; the compiler checks actual same-domain dtype.
 * Boundary k sums source [0,k) exactly and converts once. Boundary zero is +0
 * and is not an arithmetic contribution. Source NaN/Inf/zero rules match sum.
 * Requested tiles with equal non-scan regions form a fixed CPU task, reading
 * all input tiles intersecting its required source prefix. Zero-only groups
 * have no payload reads or arithmetic scratch; Empty emits no task.
 * select_outputs retains whole tasks with all reads/writes/scratch. Recompile
 * smaller output requests to shrink the prefix. Integer representability of
 * every retained write is an unchecked caller precondition. Floating overflow
 * preserves exact carry. Each invocation owns private preallocated exact
 * state and separate dense output tiles; prepared metadata is immutable.
 * Helpers return owned metadata, are pure/concurrent-safe and may throw
 * bad_alloc. Invalid static parameters return InvalidArgument; incompatible
 * schemas return TypeMismatch. Prepare checks views and scratch before entry.
 * Cancellation/resource failures prevent successful call publication; accepted
 * work retains owners through drain. Successful Results retain their storage
 * beyond context destruction. Final Result coverage follows selected outputs.
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
 * extent grows by one. Other coordinates identify independent planes. Either
 * zero boundary returns +0. Each task reads the bounding source rectangle for
 * its positive writes; zero-only tiles do not enlarge it. Input tile coverage
 * can extend beyond mathematical support. Exact row prefixes merge with exact
 * column carries before final conversion. Private scratch includes one carry
 * per required source column on the higher-numbered selected axis, reset per
 * plane. NaN priority follows original row-major order. Selection, arithmetic,
 * dtype, ownership, errors and concurrency follow prefix_sum_node. Floating
 * four-corner subtraction of rounded outputs has its own rounding error.
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
