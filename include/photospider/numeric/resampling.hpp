#pragma once

#include <array>
#include <string>
#include <type_traits>
#include <utility>

#include "photospider/numeric/curves.hpp"
#include "photospider/numeric/workflow_authoring.hpp"

namespace ps::numeric {
/** @brief Owning authoring references to independent samples and positions.
 * No runtime state or source snapshot is captured. Connect either reference to
 * later nodes or explicitly export it; construction preserves existing exports.
 */
struct ResampledSignal final {
  WorkflowNodeOutput samples;
  WorkflowNodeOutput positions;
  /** @brief Creates caller-named exports; Compiler validates their uniqueness.
   * Pure/concurrent-safe; allocation may throw bad_alloc.
   */
  std::array<WorkflowOutput, 2> outputs(
      std::string samples_name = "samples",
      std::string positions_name = "positions") const {
    return {
        {{std::move(samples_name), samples.source_node, samples.source_port},
         {std::move(positions_name), positions.source_node,
          positions.source_port}}};
  }
};
namespace resampling_detail {
inline Result<ResampledSignal> append(
    WorkflowDocument& document, WorkflowInput positions, WorkflowInput values,
    WorkflowInput new_positions, ElementType dtype, CurveDomain domain,
    CpuNumericProfile profile, bool pchip, bool multi) {
  using Answer = Result<ResampledSignal>;
  auto ids = authoring_detail::allocate_ids(document, 2,
                                            {positions, values, new_positions});
  if (!ids.ok())
    return Answer(ids.status());
  const auto helper =
      pchip ? (multi ? interpolate_pchip_multi_node : interpolate_pchip_node)
            : (multi ? interpolate_linear_multi_node : interpolate_linear_node);
  auto samples = helper(ids.value()[0], std::move(positions), std::move(values),
                        new_positions, dtype, domain, profile);
  if (!samples.ok())
    return Answer(samples.status());
  // This source-preserving identity retains facets and raw special values;
  // generic numeric layout operators intentionally erase semantic facets.
  WorkflowNode forwarded{ids.value()[1],
                         "core.identity",
                         {std::move(new_positions)},
                         {}};
  Answer result(
      ResampledSignal{{ids.value()[0], "values"}, {ids.value()[1], "value"}});
  static_assert(std::is_nothrow_move_constructible_v<WorkflowNode>);
  document.nodes.reserve(document.nodes.size() + 2);
  document.nodes.push_back(samples.take_value());
  document.nodes.push_back(std::move(forwarded));
  return result;
}
}  // namespace resampling_detail
/** @brief Resamples one scalar signal with the existing linear interpolator.
 * positions[K], values[K] (or [K,C] for _multi), new_positions[N] independently
 * accept Float32/64. K/N/C, finite samples, exact rounding and domain
 * rules follow interpolate_linear_node / interpolate_pchip_node. dtype selects
 * samples only (default Float64); positions retains the new_positions
 * descriptor, dtype, facets and raw bits. Default domain Reject, also
 * Clamp/LinearExtrapolate.
 *
 * Appends two ordinary nodes with collision-free IDs, including forward
 * references; at most 65536 total nodes. Caller serializes edits to the same
 * document. Different documents may be authored concurrently. Invalid IDs,
 * dtype/domain/profile fail InvalidArgument; actual connected metadata is
 * checked by Compiler. Allocation may throw bad_alloc. Failure/exception leaves
 * graph contents unchanged. Existing exports are untouched; outputs() is
 * explicit.
 *
 * Position-only requests independently forward requested source bits, including
 * NaN/Inf/sNaN/-0, without reading old positions/values or validating a curve.
 * Typed/upstream source validation still applies. Samples inherit Whole
 * interpolation: full input/output storage, full invalidation and Run failures
 * even for undelivered positions/columns. Resource/cancellation checks and
 * owners follow the interpolator. Empty reads no payload. Packed/aliased
 * immutable output owns its storage and metadata past context teardown. No
 * hidden cache, filtering, sample-rate inference, antialias guarantee or
 * rounded-output round-trip claim is added.
 */
inline Result<ResampledSignal> resample_linear(
    WorkflowDocument& document, WorkflowInput positions, WorkflowInput values,
    WorkflowInput new_positions, ElementType dtype = ElementType::Float64,
    CurveDomain domain = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return resampling_detail::append(document, std::move(positions),
                                   std::move(values), std::move(new_positions),
                                   dtype, domain, profile, false, false);
}
/** @brief Scalar PCHIP resampling; shares resample_linear's authoring contract.
 */
inline Result<ResampledSignal> resample_pchip(
    WorkflowDocument& document, WorkflowInput positions, WorkflowInput values,
    WorkflowInput new_positions, ElementType dtype = ElementType::Float64,
    CurveDomain domain = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return resampling_detail::append(document, std::move(positions),
                                   std::move(values), std::move(new_positions),
                                   dtype, domain, profile, true, false);
}
/** @brief Column-local linear resampling; C=1 retains its second dimension. */
inline Result<ResampledSignal> resample_linear_multi(
    WorkflowDocument& document, WorkflowInput positions, WorkflowInput values,
    WorkflowInput new_positions, ElementType dtype = ElementType::Float64,
    CurveDomain domain = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return resampling_detail::append(document, std::move(positions),
                                   std::move(values), std::move(new_positions),
                                   dtype, domain, profile, false, true);
}
/** @brief Column-local PCHIP resampling; shares the same independent positions.
 */
inline Result<ResampledSignal> resample_pchip_multi(
    WorkflowDocument& document, WorkflowInput positions, WorkflowInput values,
    WorkflowInput new_positions, ElementType dtype = ElementType::Float64,
    CurveDomain domain = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return resampling_detail::append(document, std::move(positions),
                                   std::move(values), std::move(new_positions),
                                   dtype, domain, profile, true, true);
}
}  // namespace ps::numeric
