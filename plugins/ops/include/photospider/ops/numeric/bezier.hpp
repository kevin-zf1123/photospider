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
 * @param anchors Result with one tensor member; Float32/64 sample_shape [K,2],
 * K=2..65536.
 * @param handles Result tensor [K-1,degree-1,2] of independent Float32/64
 * relative xy offsets. Quadratic/outgoing offsets are relative to the segment
 * start; cubic incoming offsets are relative to its end. Absolute controls
 * reconstruct with RN64.
 * @param start Result tensor Float32/64 [1] sampling start.
 * @param end Result tensor Float32/64 [1] sampling end, unread for count=1.
 * Each input may use any schema id/version and member key.
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
 * Output `values` is `photospider.tensor` v1/member `samples`, [count],
 * selected dtype, empty facets. Output `axis` uses the same schema/member,
 * Float64 [3], atomic_trailing_axes=1, empty facets. The Result associations
 * record active source ObjectIds. Whole execution requests active inputs with
 * Data, Validation and Descriptor (role 13). Values validates global x topology
 * and all generated coordinates, computes every value and returns full
 * certified coverage with global sample coordinates. A mathematical knot or
 * clamp reads only that anchor's y and preserves its signed zero; generic y
 * outside every evaluated stencil is numerically unused. Axis reads no
 * controls. Count=1 excludes end: values reads anchors/handles/start; axis
 * reads only start. Empty reads no payload. Each output publishes through its
 * own all-or-nothing Result transaction. Complete typed validation and upstream
 * failures apply to active inputs. Numeric domain/overflow failures have Run
 * scope; other categories are preserved. Active input edits dirty values;
 * controls never dirty axis. Output owners survive context lifetime. Sampling
 * uses exact weighted endpoints rounded to Float64. Interior results correctly
 * round the mathematical inverse Bx(t)=x and By(t) after RN64 control
 * reconstruction. Exact solving can exhaust work/capacity; failure publishes no
 * partial result. Caller fenv is preserved; no y clipping or implicit handle
 * repair is applied. Interior exact zero is +0. Whole exposes no per-value
 * numeric counters.
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
 * @param anchors Result tensor Float32/64 sample_shape [K,D], K=2..65536, D>=1.
 * @param handles Result tensor Float32/64 [K-1,degree-1,D] of relative offsets.
 * Quadratic/outgoing offsets use the start anchor; cubic incoming uses the end.
 * @param segment_indices Result tensor Int64 [N], indices in [0,K-2].
 * @param t Result tensor Float32/64 [N], finite parameters in [0,1]. Every
 * input accepts any schema id/version and member key.
 * @param degree Required fixed degree 2 or 3.
 * @param dtype Output Float32/64, default Float64.
 * @param profile Explicit CPU profile, default Strict.
 * @return Owning node metadata or InvalidArgument/InvalidDomain/Schema for
 * invalid authoring arguments. Allocation may throw bad_alloc. Compiler checks
 * input shapes/types and all logical products <=2^40 without reading payload.
 * @note Pure/concurrent-safe authoring. Output `values` is a Result using
 * schema photospider.tensor v1/member samples, shape [N,D], selected dtype and
 * empty facets. Whole execution requests all inputs with Data, Validation and
 * Descriptor (role 13), validates all query rows before arithmetic, and
 * evaluates every row/component even for a local request. Nonempty output
 * returns full certified coverage in global coordinates; the association
 * records source ObjectIds. Empty reads no payload. Mathematical endpoints use
 * one anchor; interiors use their full local control stencil. Generic controls
 * outside evaluated stencils are numerically unused. Controls reconstruct with
 * RN64; the exact polynomial rounds once to dtype. Interior exact zero is -0
 * only if every reconstructed control is -0; nonzero underflow keeps its sign.
 * Caller fenv is preserved. Invalid segment/t fails
 * InvalidArgument/InvalidDomain; used nonfinite controls fail
 * OperationFailed/InvalidDomain; actual reconstruction/output overflow fails
 * ArithmeticOverflow. Numeric failures have Run scope.
 * Typed/upstream/resource/cancellation errors retain categories. Any input edit
 * invalidates output demand. Output owner survives context teardown. Complete
 * output/workspace is admitted even for one requested cell; work/capacity
 * exhaustion fails explicitly. No geometry/color role is inferred.
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
