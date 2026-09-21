#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"
#include "photospider/data/value.hpp"

namespace ps::numeric {
/** @brief Finite sampling outside the anchor interval rejects or clamps x. */
enum class BezierDomain { Reject, Clamp };
/** @brief Authors a quadratic/cubic single-valued Bezier function sampler.
 * @param id Nonzero workflow node id.
 * @param anchors Float32/64 [K,2] dynamic xy anchors, K=2..65536.
 * @param handles Independent Float32/64 [K-1,degree-1,2] relative xy offsets.
 * Quadratic/outgoing offsets are relative to the segment start; cubic incoming
 * offsets are relative to its end. Absolute controls reconstruct with RN64.
 * @param start Independent Float32/64 [1] sampling start.
 * @param end Independent Float32/64 [1] sampling end, unread for count=1.
 * @param degree Exactly 2 or 3, fixed for the entire node.
 * @param count Endpoint-inclusive sample count, 1..1048576,
 * ascending/descending.
 * @param dtype Values dtype Float32/64, default Float64.
 * @param domain Reject (default) or Clamp; the axis is never rewritten.
 * @param profile Explicit CPU profile; unsupported hosts fail
 * BackendUnavailable.
 * @return Owned node metadata; invalid authoring parameters fail
 * InvalidArgument/InvalidDomain/Schema. Allocation can throw bad_alloc.
 * @note Helpers are pure/concurrent-safe. Compiler validates all static edges.
 * Outputs values[count] and axis:Float64[3] have empty facets and own immutable
 * packed storage beyond context lifetime. Axis is one tuple observation.
 * Values uses one Whole callback: collect complete anchors/handles and active
 * sampling scalars, validate all x topology and generated coordinates, compute
 * every value and allocate the complete output even for sparse demand. Math
 * knot/clamp uses one anchor y and preserves signed zero; generic y outside all
 * evaluated stencils is numerically unused. Axis-only reads no controls. N=1
 * ignores end; Empty reads no payload. Complete typed validation and upstream
 * failures apply to active inputs. Numeric domain/overflow failures have Run
 * scope; other error categories are preserved. Any active input edit dirties
 * values; controls never dirty axis. Output publication is all-or-nothing.
 * Sampling uses exact weighted endpoints rounded to Float64. Interior results
 * correctly round the mathematical inverse Bx(t)=x and By(t) after RN64 control
 * reconstruction. Exact algebraic/interval solving can exhaust work/capacity.
 * Caller fenv is preserved; no y clipping or implicit handle repair is applied.
 * Interior exact zero is +0. Whole does not expose per-value numeric counters.
 */
inline Result<WorkflowNode> sample_bezier_function_node(
    std::uint64_t id, WorkflowInput anchors, WorkflowInput handles,
    WorkflowInput start, WorkflowInput end, std::int64_t degree,
    std::int64_t count, ElementType dtype = ElementType::Float64,
    BezierDomain domain = BezierDomain::Reject,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  const char* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!id || !suffix || (degree != 2 && degree != 3) || count < 1 ||
      count > 1048576 ||
      (dtype != ElementType::Float32 && dtype != ElementType::Float64) ||
      (domain != BezierDomain::Reject && domain != BezierDomain::Clamp))
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               "invalid Bezier id/degree/count/dtype/domain/profile",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  return Result<WorkflowNode>(WorkflowNode{
      id,
      std::string("curve.sample_bezier_function") + suffix,
      {std::move(anchors), std::move(handles), std::move(start),
       std::move(end)},
      {{"degree", degree},
       {"count", count},
       {"dtype",
        std::string(dtype == ElementType::Float32 ? "float32" : "float64")},
       {"out_of_domain",
        std::string(domain == BezierDomain::Reject ? "reject" : "clamp")}}});
}
/** @brief Authors componentwise quadratic/cubic parametric Bezier evaluation.
 * @param id Nonzero workflow node id.
 * @param anchors Float32/64 [K,D], K=2..65536, D>=1.
 * @param handles Independent Float32/64 [K-1,degree-1,D] relative offsets.
 * Quadratic/outgoing offsets use the start anchor; cubic incoming uses the end.
 * @param segment_indices Int64 [N] dynamic segment indices, each in [0,K-2].
 * @param t Independent Float32/64 [N] dynamic parameters, finite in [0,1].
 * @param degree Required fixed degree 2 or 3.
 * @param dtype Output Float32/64, default Float64.
 * @param profile Explicit CPU profile, default Strict.
 * @return Owning node metadata or InvalidArgument/InvalidDomain/Schema for
 * invalid authoring arguments. Allocation may throw bad_alloc. Compiler checks
 * input shapes/types and all logical products <=2^40 without reading payload.
 * @note Pure/concurrent-safe authoring. Output values[N,D] has empty facets,
 * per-cell observations and owned immutable fragments surviving context
 * teardown. No geometry/color role, monotonicity, global topology or clipping
 * is inferred. Requested row controls precede local component controls;
 * endpoints read only their anchor, interiors retain all selected
 * anchors/handles and typed closure. Controls reconstruct with RN64; the exact
 * polynomial rounds once to dtype. Interior exact zero is -0 only if every
 * reconstructed control is -0; nonzero underflow keeps its sign. Caller
 * floating environment is unchanged. Invalid requested indices/t fail
 * InvalidArgument/InvalidDomain; nonfinite demanded controls fail
 * OperationFailed/InvalidDomain; actual reconstruction or output overflow fails
 * OperationFailed/ArithmeticOverflow. All identify the dependent Atom;
 * typed/upstream/resource/cancellation errors are preserved. Cache witnesses
 * include selected row controls and local component support. Empty reads
 * nothing; bounded resources/work can fail explicitly.
 */
inline Result<WorkflowNode> evaluate_bezier_node(
    std::uint64_t id, WorkflowInput anchors, WorkflowInput handles,
    WorkflowInput segment_indices, WorkflowInput t, std::int64_t degree,
    ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  const char* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!id || !suffix || (degree != 2 && degree != 3) ||
      (dtype != ElementType::Float32 && dtype != ElementType::Float64))
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               "invalid parametric Bezier id/degree/dtype/profile",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  return Result<WorkflowNode>(WorkflowNode{
      id,
      std::string("curve.evaluate_bezier") + suffix,
      {std::move(anchors), std::move(handles), std::move(segment_indices),
       std::move(t)},
      {{"degree", degree},
       {"dtype",
        std::string(dtype == ElementType::Float32 ? "float32" : "float64")}}});
}
}  // namespace ps::numeric
