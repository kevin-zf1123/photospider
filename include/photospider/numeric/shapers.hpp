#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/numeric/arrays.hpp"
#include "photospider/numeric/workflow_authoring.hpp"

namespace ps::numeric {
namespace shaper_detail {
inline const char* suffix(CpuNumericProfile profile) {
  return profile == CpuNumericProfile::Strict ? "_strict"
         : profile == CpuNumericProfile::AppleSiliconNeon
             ? "_accelerated_apple_silicon"
         : profile == CpuNumericProfile::X86Avx2 ? "_accelerated_x86_64"
                                                 : nullptr;
}
inline Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline Result<WorkflowNodeOutput> linear(
    WorkflowDocument& document, WorkflowInput input, WorkflowInput lower,
    WorkflowInput upper, const ValueDescriptor& descriptor,
    CpuNumericProfile profile, bool inverse) {
  using Answer = Result<WorkflowNodeOutput>;
  const auto* selected = suffix(profile);
  if (!selected)
    return Answer(invalid("invalid shaper profile"));
  if ((descriptor.element_type != ElementType::Float32 &&
       descriptor.element_type != ElementType::Float64) ||
      descriptor.shape.empty() || descriptor.shape.size() > 8)
    return Answer(Status{ErrorCode::TypeMismatch,
                         "shaper requires Float32/64 rank 1..8",
                         FailureReason::None,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  std::uint64_t elements = 1;
  for (auto extent : descriptor.shape) {
    if (!extent || extent > (UINT64_C(1) << 40) / elements)
      return Answer(invalid("shaper shape exceeds 2^40 values"));
    elements *= extent;
  }
  const bool narrow = descriptor.element_type == ElementType::Float32;
  const unsigned count = 7 + (narrow ? 2 : 0) + (inverse ? 1 : 0);
  auto ids =
      authoring_detail::allocate_ids(document, count, {input, lower, upper});
  if (!ids.ok())
    return Answer(ids.status());
  unsigned next = 0;
  std::vector<WorkflowNode> nodes;
  nodes.reserve(count);
  std::array<WorkflowNodeOutput, 2> constants;
  for (unsigned i = 0; i < 2; ++i) {
    const auto id = ids.value()[next++];
    nodes.push_back(
        {id, "core.constant", {}, {{"value", static_cast<double>(i)}}});
    constants[i] = {id, "value"};
  }
  // Only exact 0/1 literals pass through the existing scalar cast. Constant
  // views, bound broadcasts and remap all select the requested CPU profile.
  if (narrow) {
    for (auto& constant : constants) {
      const auto id = ids.value()[next++];
      nodes.push_back({id,
                       "numeric.cast",
                       {constant},
                       {{"dtype", std::string("float32")},
                        {"rounding", std::string("ties_even")},
                        {"overflow", std::string("reject")}}});
      constant = {id, "value"};
    }
  }
  const auto remap = std::string("numeric.remap_range") + selected;
  WorkflowInput checked_lower = lower;
  if (inverse) {
    const auto id = ids.value()[next++];
    // The scalar source interval forces lower<upper even though the final
    // inverse remap would otherwise permit unordered target bounds.
    nodes.push_back({id, remap, {lower, lower, upper, lower, lower}, {}});
    checked_lower = WorkflowNodeOutput{id, "values"};
  }
  std::array<WorkflowInput, 4> sources{checked_lower, upper, constants[0],
                                       constants[1]};
  std::array<WorkflowNodeOutput, 4> expanded;
  for (unsigned i = 0; i < 4; ++i) {
    const auto id = ids.value()[next++];
    // constant_node requires its source to be exactly [1], so actual scalar
    // bounds are verified by Compiler independently of the shape hint.
    auto view = constant_node(id, sources[i], descriptor.shape,
                              ArrayLayout::View, profile);
    if (!view.ok())
      return Answer(view.status());
    nodes.push_back(view.take_value());
    expanded[i] = {id, "values"};
  }
  const auto output = ids.value()[next++];
  nodes.push_back(
      {output,
       remap,
       inverse ? std::vector<WorkflowInput>{input, expanded[2], expanded[3],
                                            expanded[0], expanded[1]}
               : std::vector<WorkflowInput>{input, expanded[0], expanded[1],
                                            expanded[2], expanded[3]},
       {}});
  static_assert(std::is_nothrow_move_constructible_v<WorkflowNode>);
  Answer result(WorkflowNodeOutput{output, "values"});
  document.nodes.reserve(document.nodes.size() + nodes.size());
  for (auto& node : nodes)
    document.nodes.push_back(std::move(node));
  return result;
}
inline Result<WorkflowNode> logarithmic(std::uint64_t id, bool inverse,
                                        WorkflowInput input,
                                        WorkflowInput lower,
                                        WorkflowInput upper,
                                        CpuNumericProfile profile) {
  const auto* selected = suffix(profile);
  if (!id || !selected)
    return Result<WorkflowNode>(invalid("invalid log shaper id/profile"));
  return Result<WorkflowNode>(WorkflowNode{
      id,
      std::string(inverse ? "curve.log2_shaper_inverse" : "curve.log2_shaper") +
          selected,
      {std::move(input), std::move(lower), std::move(upper)},
      {}});
}
}  // namespace shaper_detail
/** @brief Expands a linear [lower,upper] to [0,1] coordinate shaper.
 * @param document Caller-owned graph; edits to one graph must be serialized.
 * @param input Dynamic Float32/64 rank 1..8, positive shape, <=2^40 values.
 * @param lower Dynamic [1] scalar with the same dtype as input.
 * @param upper Dynamic [1] scalar, finite and strictly greater than lower.
 * @param descriptor Authoring input shape/dtype hint; Compiler checks actual
 * remap edges and scalar bounds. Mismatches fail TypeMismatch before payload.
 * @param profile CPU suffix used by all numeric views and remap operations.
 * @return Connectable values reference. Adds only ordinary nodes, leaving
 * document outputs unchanged. IDs avoid all existing/referenced producer IDs.
 * Invalid authoring fails Schema status; allocation may throw bad_alloc.
 * Failure/exception leaves the graph unchanged. Different graphs are safe to
 * author concurrently. No payload, registry or runtime resource is accessed.
 * @note Runtime preserves shape/dtype with empty facets. The complete remap
 * formula rounds once, extends without clipping and preserves its IEEE special
 * values/zero rules. Every nonempty request validates both finite ordered
 * bounds even for NaN/endpoints, while input reads and dirty witnesses stay
 * local with typed Validation closure. Empty reads no dynamic payload. Invalid
 * bounds use InvalidArgument/InvalidDomain; upstream, backend, work, capacity,
 * cancellation and stale failures retain their categories. Output owners
 * outlive context; all expanded views/intermediates remain subject to host
 * resource admission.
 */
inline Result<WorkflowNodeOutput> linear_shaper(
    WorkflowDocument& document, WorkflowInput input, WorkflowInput lower,
    WorkflowInput upper, const ValueDescriptor& descriptor,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return shaper_detail::linear(document, std::move(input), std::move(lower),
                               std::move(upper), descriptor, profile, false);
}
/** @brief Expands [0,1] to [lower,upper], including a scalar lower<upper guard.
 * Shares linear_shaper's authoring, IEEE, demand and ownership contract.
 * Finite coordinates outside [0,1] extrapolate; infinities retain their signs.
 */
inline Result<WorkflowNodeOutput> linear_shaper_inverse(
    WorkflowDocument& document, WorkflowInput input, WorkflowInput lower,
    WorkflowInput upper, const ValueDescriptor& descriptor,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return shaper_detail::linear(document, std::move(input), std::move(lower),
                               std::move(upper), descriptor, profile, true);
}
/** @brief Whole-expression log coordinate
 * (ln(input)-ln(lower))/(ln(upper)-ln(lower)). Ordered inputs share Float32/64
 * dtype; lower/upper are [1] and require 0<lower<upper before any input
 * special-value handling. Output values has input shape/dtype and empty facets.
 * No static mode or clipping parameter. Construction is pure/thread-safe and
 * may throw bad_alloc; invalid id/profile fails
 * InvalidArgument/InvalidDomain/Schema. Endpoints return +0/1 exactly; input
 * NaN retains its sign/payload and is quieted, either zero returns -Inf,
 * negative input returns canonical quiet NaN, and +Inf returns +Inf.
 * @note Every profile currently correctly rounds the complete expression.
 * Accelerated profiles report a strict scalar fallback when entering certified
 * interval arithmetic. Together with exact branches this preserves monotonicity
 * independently of request order. Refinement uses 128..4096 fraction bits;
 * unresolved rounding fails ResourceExhausted/CapacityLimit. Work/cancellation
 * can interrupt refinement and retain their original failure categories.
 * @note Every nonempty observation requires both scalar bounds (Control and
 * typed Validation) before local input (Data and typed Validation). Invalid
 * bounds fail InvalidArgument/InvalidDomain with the offending port and Atom.
 * Empty reads no payload. Source/bound witnesses participate in cache identity.
 * Immutable packed output owners outlive the execution context; arbitrary legal
 * source strides are supported. State, scratch, dependencies and output storage
 * use host resource admission. Upstream errors retain their provenance.
 */
inline Result<WorkflowNode> log2_shaper_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput lower,
    WorkflowInput upper,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return shaper_detail::logarithmic(
      id, false, std::move(input), std::move(lower), std::move(upper), profile);
}
/** @brief Whole-expression lower*(upper/lower)^input, with exact t=0/1
 * endpoints. Shares log2_shaper_node's interface and authoring contract.
 * Positive finite bounds precede NaN propagation; inverse -Inf maps to +0, +Inf
 * to +Inf.
 */
inline Result<WorkflowNode> log2_shaper_inverse_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput lower,
    WorkflowInput upper,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return shaper_detail::logarithmic(
      id, true, std::move(input), std::move(lower), std::move(upper), profile);
}
}  // namespace ps::numeric
