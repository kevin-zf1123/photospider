#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/numeric/bezier.hpp"
#include "photospider/numeric/curves.hpp"
#include "photospider/numeric/expression.hpp"
#include "photospider/numeric/sequences.hpp"
#include "photospider/numeric/workflow_authoring.hpp"

namespace ps::numeric {
/** @brief Owning authoring references to a generic baked table and its axis.
 * Contains no runtime Value or frozen state. Connect these references to later
 * nodes or explicitly export them with outputs(). The template does not modify
 * WorkflowDocument::outputs or claim a bound on table interpolation error.
 */
struct BakedLut1d final {
  WorkflowNodeOutput values;
  WorkflowNodeOutput axis;
  /** @brief Creates two caller-named exports, default values and axis.
   * Compiler checks nonempty/unique names in the complete document. Allocation
   * may throw bad_alloc; this pure method can be called concurrently.
   */
  std::array<WorkflowOutput, 2> outputs(std::string values_name = "values",
                                        std::string axis_name = "axis") const {
    return {{{std::move(values_name), values.source_node, values.source_port},
             {std::move(axis_name), axis.source_node, axis.source_port}}};
  }
};
namespace lut1d_detail {
inline Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline Status endpoints(const SequenceInput& start, const SequenceInput& end) {
  for (const auto* value : {&start, &end})
    if (value->descriptor.shape != std::vector<std::uint64_t>{1} ||
        (value->descriptor.element_type != ElementType::Float32 &&
         value->descriptor.element_type != ElementType::Float64))
      return invalid("LUT baking endpoints require Float32/64 [1]");
  return Status::success();
}
inline Result<BakedLut1d> append(WorkflowDocument* document,
                                 std::vector<WorkflowNode> nodes) {
  // Construct all exported metadata before the only document mutation. Reserve
  // can throw before insertion; subsequent moves cannot fail or allocate.
  static_assert(std::is_nothrow_move_constructible_v<WorkflowNode>);
  Result<BakedLut1d> result(
      BakedLut1d{{nodes.back().id, "values"}, {nodes.front().id, "axis"}});
  document->nodes.reserve(document->nodes.size() + nodes.size());
  for (auto& node : nodes)
    document->nodes.push_back(std::move(node));
  return result;
}
inline Result<BakedLut1d> interpolation(WorkflowDocument& document,
                                        WorkflowInput x, WorkflowInput y,
                                        SequenceInput start, SequenceInput end,
                                        std::int64_t count, ElementType dtype,
                                        CurveDomain domain,
                                        CpuNumericProfile profile, bool pchip,
                                        bool multi) {
  using Answer = Result<BakedLut1d>;
  auto valid = endpoints(start, end);
  if (!valid.ok())
    return Answer(valid);
  auto ids = authoring_detail::allocate_ids(document, 2,
                                            {x, y, start.source, end.source});
  if (!ids.ok())
    return Answer(ids.status());
  auto query = linspace_node(ids.value()[0], std::move(start), std::move(end),
                             count, ElementType::Float64, profile);
  if (!query.ok())
    return Answer(query.status());
  const auto helper =
      pchip ? (multi ? interpolate_pchip_multi_node : interpolate_pchip_node)
            : (multi ? interpolate_linear_multi_node : interpolate_linear_node);
  auto sampled = helper(ids.value()[1], std::move(x), std::move(y),
                        WorkflowNodeOutput{ids.value()[0], "values"}, dtype,
                        domain, profile);
  if (!sampled.ok())
    return Answer(sampled.status());
  std::vector<WorkflowNode> nodes;
  nodes.reserve(2);
  nodes.push_back(query.take_value());
  nodes.push_back(sampled.take_value());
  return append(&document, std::move(nodes));
}
}  // namespace lut1d_detail
/** @brief Shared authoring contract for the six LUT1D baking templates.
 * @param document Caller-owned graph receiving one or two ordinary nodes.
 * Caller serializes concurrent edits to the same graph; different graphs are
 * independent. Existing unique nonzero IDs and all referenced producer IDs are
 * reserved, including references supplied by this call. New IDs are the lowest
 * unused positive numbers; at most 65536 nodes may exist after expansion.
 * @param start Dynamic Float32/64 [1] endpoint reference plus metadata hint.
 * @param end Independent Float32/64 [1] endpoint, statically checked even when
 * count=1. Compiler checks actual connections, not only these hints.
 * @param count Required endpoint-inclusive count in [1,1048576].
 * @param dtype Table Float32/64, default Float64.
 * @param profile Explicit key suffix for every node; default Strict.
 * @return Table/axis authoring references; invalid parameters fail existing
 * helper Status, with invalid template endpoints/IDs failing InvalidArgument.
 * Allocation may throw bad_alloc. Failure or exception leaves graph contents
 * unchanged. Output declarations are added only by the caller using outputs().
 * @note Construction reads no payload, freezes nothing and writes no files.
 * Runtime obeys expanded source contracts: output values[count] or [count,C]
 * and axis:Float64[3], independently demanded, immutable owners after context
 * teardown, source-specific finite/grid/domain/rounding rules and exact dirty
 * witnesses. N=1 ignores end payload. Interpolation uses Float64 linspace
 * queries regardless of table dtype. Source numerical, typed, upstream,
 * backend, resource, cancellation and stale failures retain their identities;
 * no private cache or table approximation guarantee is added. Unsupported
 * profiles fail at source compile/preflight. Use explicit host budgets for
 * runtime execution.
 */
/** @brief Bakes a bounded expression; derives coefficient names from source.
 * expression and coefficients follow sample_expression_node. Shared contract
 * above applies. Expanded sampler supplies both outputs.
 */
inline Result<BakedLut1d> bake_lut1d_expression(
    WorkflowDocument& document, std::string expression, SequenceInput start,
    SequenceInput end, std::int64_t count,
    const std::map<std::string, WorkflowInput>& coefficients = {},
    ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  using Answer = Result<BakedLut1d>;
  auto valid = lut1d_detail::endpoints(start, end);
  if (!valid.ok())
    return Answer(valid);
  std::vector<WorkflowInput> inputs{start.source, end.source};
  for (const auto& coefficient : coefficients)
    inputs.push_back(coefficient.second);
  auto ids = authoring_detail::allocate_ids(document, 1, inputs);
  if (!ids.ok())
    return Answer(ids.status());
  auto node = sample_expression_node(
      ids.value()[0], std::move(expression), std::move(start.source),
      std::move(end.source), count, coefficients, dtype, profile);
  if (!node.ok())
    return Answer(node.status());
  std::vector<WorkflowNode> nodes;
  nodes.push_back(node.take_value());
  return lut1d_detail::append(&document, std::move(nodes));
}
/** @brief Bakes a scalar Bezier function; degree is 2/3, default domain Reject.
 * anchors/handles and RN64 reconstruction follow sample_bezier_function_node.
 * Shared contract above applies. Expanded sampler supplies both outputs.
 */
inline Result<BakedLut1d> bake_lut1d_bezier(
    WorkflowDocument& document, WorkflowInput anchors, WorkflowInput handles,
    SequenceInput start, SequenceInput end, std::int64_t degree,
    std::int64_t count, ElementType dtype = ElementType::Float64,
    BezierDomain domain = BezierDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  using Answer = Result<BakedLut1d>;
  auto valid = lut1d_detail::endpoints(start, end);
  if (!valid.ok())
    return Answer(valid);
  auto ids = authoring_detail::allocate_ids(
      document, 1, {anchors, handles, start.source, end.source});
  if (!ids.ok())
    return Answer(ids.status());
  auto node = sample_bezier_function_node(
      ids.value()[0], std::move(anchors), std::move(handles),
      std::move(start.source), std::move(end.source), degree, count, dtype,
      domain, profile);
  if (!node.ok())
    return Answer(node.status());
  std::vector<WorkflowNode> nodes;
  nodes.push_back(node.take_value());
  return lut1d_detail::append(&document, std::move(nodes));
}
/** @brief Bakes scalar linear interpolation after Float64 linspace.
 * x/y and domain follow interpolate_linear_node; default domain Reject.
 * Shared baking contract above applies; C=1 is retained for multi inputs.
 */
inline Result<BakedLut1d> bake_lut1d_linear(
    WorkflowDocument& document, WorkflowInput x, WorkflowInput y,
    SequenceInput start, SequenceInput end, std::int64_t count,
    ElementType dtype = ElementType::Float64,
    CurveDomain domain = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return lut1d_detail::interpolation(document, std::move(x), std::move(y),
                                     std::move(start), std::move(end), count,
                                     dtype, domain, profile, false, false);
}
/** @brief Bakes scalar PCHIP interpolation after Float64 linspace.
 * x/y and domain follow interpolate_pchip_node; default domain Reject.
 * Shared baking contract above applies; C=1 is retained for multi inputs.
 */
inline Result<BakedLut1d> bake_lut1d_pchip(
    WorkflowDocument& document, WorkflowInput x, WorkflowInput y,
    SequenceInput start, SequenceInput end, std::int64_t count,
    ElementType dtype = ElementType::Float64,
    CurveDomain domain = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return lut1d_detail::interpolation(document, std::move(x), std::move(y),
                                     std::move(start), std::move(end), count,
                                     dtype, domain, profile, true, false);
}
/** @brief Bakes column-local linear interpolation after Float64 linspace.
 * x/y and domain follow interpolate_linear_multi_node; default domain Reject.
 * Shared baking contract above applies; C=1 is retained for multi inputs.
 */
inline Result<BakedLut1d> bake_lut1d_linear_multi(
    WorkflowDocument& document, WorkflowInput x, WorkflowInput y,
    SequenceInput start, SequenceInput end, std::int64_t count,
    ElementType dtype = ElementType::Float64,
    CurveDomain domain = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return lut1d_detail::interpolation(document, std::move(x), std::move(y),
                                     std::move(start), std::move(end), count,
                                     dtype, domain, profile, false, true);
}
/** @brief Bakes column-local PCHIP interpolation after Float64 linspace.
 * x/y and domain follow interpolate_pchip_multi_node; default domain Reject.
 * Shared baking contract above applies; C=1 is retained for multi inputs.
 */
inline Result<BakedLut1d> bake_lut1d_pchip_multi(
    WorkflowDocument& document, WorkflowInput x, WorkflowInput y,
    SequenceInput start, SequenceInput end, std::int64_t count,
    ElementType dtype = ElementType::Float64,
    CurveDomain domain = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return lut1d_detail::interpolation(document, std::move(x), std::move(y),
                                     std::move(start), std::move(end), count,
                                     dtype, domain, profile, true, true);
}
/** @brief Shared contract for scalar and per-channel LUT1D application.
 * Input is Float32/64 rank 1..8, table is independent Float32/64 [L] or
 * [L,C], axis is Float64[3]; L=1..1048576 and products <=2^40. Channels use
 * the final input axis, including rank-1 [C] and C=1. Output values preserves
 * input shape with empty facets. input_type is an authoring hint used only for
 * the default output dtype; Compiler validates the actual graph edges.
 * Helpers are pure/concurrent-safe and own metadata; allocation may throw
 * bad_alloc. Invalid authoring parameters fail InvalidArgument/InvalidDomain.
 * Runtime validates the full endpoint-weighted RN64 grid and axis step before
 * input queries, then reads only selected singleton/pair table entries and
 * typed Validation. Singleton axes require bit-identical endpoints and +0 step.
 * Every query is read even for constant tables; invalid axes/queries/demanded
 * entries fail OperationFailed/InvalidDomain at the dependent Atom. Actual
 * destination overflow fails ArithmeticOverflow; host/typed/upstream errors
 * retain their identity. Descending axes and all CurveDomain policies work.
 * Exact selection preserves zero sign. Strict rounds complete linear formulas
 * once, with -0 exact zero only for two -0 endpoints. Accelerated follows
 * CpuNumericProfile's final FP32 bound and preserves caller fenv. Owned packed
 * fragments survive context teardown. Empty reads nothing. Global axis work,
 * exact scratch, optional grid and per-request certificates are bounded by host
 * budgets. Cache witnesses retain complete axis, selected input and local table
 * coordinates. No approximation quality bound relative to the table's
 * generating function is inferred.
 */
/** @brief Applies one scalar table to every requested input element. */
inline Result<WorkflowNode> apply_lut1d_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput table,
    WorkflowInput axis, ElementType input_type,
    std::optional<ElementType> dtype = {},
    CurveDomain domain = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  if (input_type != ElementType::Float32 && input_type != ElementType::Float64)
    return Result<WorkflowNode>(
        lut1d_detail::invalid("LUT1D input dtype hint requires Float32/64"));
  return curve_detail::node(id, "apply_lut1d", std::move(input),
                            std::move(table), std::move(axis),
                            dtype.value_or(input_type), domain, profile);
}
/** @brief Applies independent table columns to corresponding input channels. */
inline Result<WorkflowNode> apply_lut1d_channels_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput table,
    WorkflowInput axis, ElementType input_type,
    std::optional<ElementType> dtype = {},
    CurveDomain domain = CurveDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  if (input_type != ElementType::Float32 && input_type != ElementType::Float64)
    return Result<WorkflowNode>(
        lut1d_detail::invalid("LUT1D input dtype hint requires Float32/64"));
  return curve_detail::node(id, "apply_lut1d_channels", std::move(input),
                            std::move(table), std::move(axis),
                            dtype.value_or(input_type), domain, profile);
}
}  // namespace ps::numeric
