#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/data/tensor_span.hpp"
#include "photospider/ops/numeric/diagnostics.hpp"

namespace ps::numeric {
/** @brief Requires an in-domain query or clamps to an endpoint value. */
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
 * selected dtype, ordinary data. Output `axis` uses the same schema/member,
 * Float64 [3], atomic_trailing_axes=1, ordinary data.
 * Each selected values tile has one fixed CPU task reading complete controls,
 * start and, for count>1, end. Axis is an independent scalar-only task; count=1
 * excludes end and uses zero scratch for axis. All schemas remain checked.
 * Values use fixed preallocated ExactBezier/ExactSampling scratch, direct
 * anchor search and selected-segment RN64 reconstruction. Only selected output
 * tiles are computed and allocated. Empty reads no payload.
 *
 * Active endpoints must be finite and distinct for count>1. Axis selection
 * requires finite nonzero correctly directed step; values selection requires
 * each selected RN64 coordinate to differ from its valid neighbors. Values
 * additionally require globally finite/increasing anchor x and finite x
 * offsets/reconstructions, nonnegative segment x derivatives, valid reject
 * domain, finite used y/reconstructions and representable finite outputs.
 * These are unchecked caller preconditions. Axis-only imposes no control or
 * selected-coordinate separation requirements. Declared upstream tiles still
 * obey their producer's own contract, including mathematically unused y.
 *
 * Exact anchor/clamp converts one y and preserves zero sign. Interiors solve
 * the mathematical Bx(t)=q and round By(t) once after RN64 control
 * construction; exact interior zero is +0. Refinement/capacity guards remain
 * operational errors. Caller fenv is preserved; cancellation is checked per
 * value/search and in exact arithmetic. Prepared views are borrowed during
 * invocation. Runs share immutable preparation and own scratch/output. Required
 * tasks share one call's ordered publication result, including joint
 * values/axis. Retained owners survive context teardown. Compatible rebinding
 * reuses preparation; content caching and repeated Result identity are outside
 * this contract. No per-value solver counters or y clipping are provided.
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
 * ordinary axes. Each selected output tile has one fixed CPU task, reading
 * its segment/t rows and all anchor/handle rows for its selected columns,
 * closed through each source grid. Dynamic segment indices retain a constant-
 * time address guard; invalid indices fail InvalidArgument/InvalidDomain.
 * Finite t in [0,1], finite consumed controls, finite RN64 reconstructions and
 * finite representable selected outputs are unchecked caller preconditions.
 * Endpoints numerically use one anchor; interiors use their local controls.
 * Generic unused controls have no additional finite requirement. Declared
 * upstream producer tiles must still satisfy their own contracts, including
 * handles even when every t selects an endpoint.
 *
 * Float32 inputs widen exactly. Each absolute handle reconstructs with RN64,
 * then the whole polynomial rounds once to dtype. Interior exact zero is -0
 * only when every reconstructed control is -0; nonzero underflow keeps its
 * sign. Current profiles use the same exact arithmetic. Fenv is preserved.
 * Each invocation borrows prepared views and owns fixed preallocated
 * ExactPolynomial/ExactSampling scratch. It computes and allocates only
 * selected output tiles. Runs share immutable preparation and have separate
 * scratch/output. Cancellation is checked per row/component and in arithmetic.
 * Static, binding, backend, resource and cancellation errors retain their
 * categories; numerical precondition violations have no promised diagnostic.
 * Successful public call publication waits for all required tasks and ordered
 * retirement. Empty reads no payload. Retained output owners survive context
 * teardown. Compatible rebinding reuses preparation; content caching and
 * repeated Result identity are outside this contract.
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
