#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/ops/numeric/arrays.hpp"
#include "photospider/ops/numeric/diagnostics.hpp"
#include "photospider/ops/numeric/sequences.hpp"
#include "photospider/ops/numeric/workflow_authoring.hpp"

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
    WorkflowInput upper, const TensorDescriptor& descriptor,
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
  // Generate exact Float32 0/1 singletons with the existing sequence primitive.
  // This is literal construction, not a replacement format-conversion API.
  if (narrow) {
    SchemaTemplate schema;
    schema.id = "photospider.tensor";
    ResultTensorSpec member;
    member.key = "samples";
    member.descriptor = {ElementType::Float64, {1}};
    schema.tensors.push_back(std::move(member));
    auto scalar_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    for (auto& constant : constants) {
      const auto id = ids.value()[next++];
      const SequenceInput scalar{constant, scalar_schema};
      auto generated =
          linspace_node(id, scalar, scalar, 1, ElementType::Float32, profile);
      if (!generated.ok())
        return Answer(generated.status());
      nodes.push_back(generated.take_value());
      constant = {id, "values"};
    }
  }
  const auto remap = std::string("numeric.remap_range") + selected;
  WorkflowInput checked_lower = lower;
  if (inverse) {
    const auto id = ids.value()[next++];
    // Keep the scalar bound dependency in the inverse composition. Ordered
    // finite bounds are a caller precondition of the fixed remap kernel.
    nodes.push_back({id, remap, {lower, lower, upper, lower, lower}, {}});
    checked_lower = WorkflowNodeOutput{id, "values"};
  }
  std::array<WorkflowInput, 4> sources{checked_lower, upper, constants[0],
                                       constants[1]};
  std::array<WorkflowNodeOutput, 4> expanded;
  for (unsigned i = 0; i < 4; ++i) {
    const auto id = ids.value()[next++];
    // constant_node requires its source to be exactly [1], so actual scalar
    // input shapes are verified by Compiler independently of the shape hint.
    auto expanded_node =
        constant_node(id, sources[i], descriptor.shape, profile);
    if (!expanded_node.ok())
      return Answer(expanded_node.status());
    nodes.push_back(expanded_node.take_value());
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
 * @param input Result with one tensor member under any schema id/version/key;
 * its complete `sample_shape()` is Float32/64, rank 1..8, positive and <=2^40
 * elements.
 * @param lower Result with one same-dtype finite tensor of `sample_shape()`
 * [1].
 * @param upper Result with one same-dtype tensor of `sample_shape()` [1],
 * finite and strictly greater than the finite lower bound.
 * @param descriptor Authoring input shape/dtype hint; Compiler checks actual
 * Result edges and scalar bound schemas. Mismatches fail TypeMismatch before
 * payload.
 * @param profile CPU suffix used by all constant and remap operations.
 * @return Connectable values reference. Adds only ordinary nodes, leaving
 * document outputs unchanged. IDs avoid all existing/referenced producer IDs.
 * Invalid authoring fails Schema status; allocation may throw bad_alloc.
 * Failure/exception leaves the graph unchanged. Different graphs are safe to
 * author concurrently. No payload, registry or runtime resource is accessed.
 * @note Runtime returns `values` as an immutable Result with
 * `photospider.tensor` v1/member `samples`, preserving shape/dtype. The
 * complete remap formula rounds once, extends without clipping and
 * preserves its IEEE special values/zero rules. Finite ordered bounds are an
 * unchecked caller precondition. Fixed constant/remap tasks compute selected
 * dense tiles; Empty issues no payload work. Operational resource/backend and
 * cancellation failures prevent successful publication. Output owners outlive
 * context; all selected intermediates enter block resource admission.
 * Float32 literal creation additionally depends on linspace availability.
 */
inline Result<WorkflowNodeOutput> linear_shaper(
    WorkflowDocument& document, WorkflowInput input, WorkflowInput lower,
    WorkflowInput upper, const TensorDescriptor& descriptor,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return shaper_detail::linear(document, std::move(input), std::move(lower),
                               std::move(upper), descriptor, profile, false);
}
/** @brief Expands [0,1] to caller-supplied finite ordered [lower,upper].
 * `input`, `lower` and `upper` are single-tensor Results with arbitrary schema
 * ids/member keys and the same Float32/64 dtype. Output is a
 * `photospider.tensor` v1 `samples` Result. Shares linear_shaper's authoring,
 * IEEE, unchecked-bound, selection and ownership contract.
 * Finite coordinates outside [0,1] extrapolate; infinities retain their signs.
 */
inline Result<WorkflowNodeOutput> linear_shaper_inverse(
    WorkflowDocument& document, WorkflowInput input, WorkflowInput lower,
    WorkflowInput upper, const TensorDescriptor& descriptor,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return shaper_detail::linear(document, std::move(input), std::move(lower),
                               std::move(upper), descriptor, profile, true);
}
/** @brief Whole-expression (ln(input)-ln(lower))/(ln(upper)-ln(lower)).
 * Ordered input/lower/upper Results each contain one tensor member under any
 * schema id/version/key. Full sample_shape includes batch axes; input rank is
 * 1..8, positive extents and <=2^40 elements. All ports share Float32/64 dtype;
 * bounds have shape [1]. Finite 0<lower<upper is an unchecked caller
 * precondition. Output values uses photospider.tensor v1/member samples with
 * the complete input shape/dtype as ordinary axes and no domain description. No
 * static mode or clipping parameter. Construction is pure/thread-safe and may
 * throw bad_alloc; invalid id/profile fails InvalidArgument/InvalidDomain.
 * Input NaNs preserve sign/payload and are quieted. Both zeros return -Inf,
 * negative inputs canonical quiet NaN, +Inf returns +Inf; endpoints give +0/1.
 * Overflow/underflow remain successful IEEE values. Current profiles correctly
 * round the whole expression and are monotone for fixed valid bounds.
 *
 * Each selected output tile has one fixed CPU task reading its input region
 * and both scalar bounds, closed through source grids. Only selected output is
 * computed/allocated. ExactShaper scratch is fixed per invocation; checked
 * arithmetic state carries retry/range/failure without exception allocation.
 * Refinement uses 128..4096 fractional bits; unresolved precision/capacity
 * fails ResourceExhausted. Cancellation is checked in output and arithmetic
 * work. A zero-significand guard protects integer decomposition, not full bound
 * validation; invalid numerical bounds have no diagnostic precedence guarantee.
 *
 * Preparation validates metadata, exact source slots, output and scratch.
 * Views are borrowed during invocation; Runs share immutable preparation and
 * own scratch/output. Legal affine strides/offsets are supported. Caller fenv
 * is preserved. Empty reads no payload. Required tasks share one call's ordered
 * publication outcome; output owners survive context teardown. Compatible
 * rebinding reuses preparation; content caching/repeated identity and per-value
 * diagnostic counters are outside this contract. Operational static, binding,
 * resource, backend and cancellation errors retain their categories.
 */
inline Result<WorkflowNode> log2_shaper_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput lower,
    WorkflowInput upper,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return shaper_detail::logarithmic(
      id, false, std::move(input), std::move(lower), std::move(upper), profile);
}
/** @brief Whole-expression lower*(upper/lower)^input.
 * Shares log2_shaper_node's schemas, unchecked finite positive ordered-bound
 * precondition, fixed execution, precision, ownership and authoring contract.
 * Input +/-0 returns lower bits and input=1 returns upper bits. NaNs preserve
 * sign/payload and are quieted; -Inf maps to +0, +Inf to +Inf. Finite inputs
 * can produce successful positive overflow/underflow values without clipping.
 */
inline Result<WorkflowNode> log2_shaper_inverse_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput lower,
    WorkflowInput upper,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return shaper_detail::logarithmic(
      id, true, std::move(input), std::move(lower), std::move(upper), profile);
}
}  // namespace ps::numeric
