#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/numeric_diagnostics.hpp"

namespace ps::numeric {
/** @brief Explicit discrete/continuous boundary extension. */
enum class LowpassBoundary { Reflect, Replicate, Zero, Wrap };
namespace lowpass_detail {
inline bool parameter(double value, bool zero = false) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, 8);
  const auto magnitude = bits & UINT64_C(0x7fffffffffffffff);
  return magnitude < UINT64_C(0x7ff0000000000000) &&
         (!(bits >> 63) || !magnitude) && (zero || magnitude);
}
inline Result<WorkflowNode> uniform(std::uint64_t id, const char* kernel,
                                    WorkflowInput input, std::int64_t axis,
                                    std::int64_t radius, double cutoff_or_sigma,
                                    double beta, LowpassBoundary boundary,
                                    CpuNumericProfile profile, bool gaussian,
                                    bool kaiser) {
  const auto* extension = boundary == LowpassBoundary::Reflect     ? "reflect"
                          : boundary == LowpassBoundary::Replicate ? "replicate"
                          : boundary == LowpassBoundary::Zero      ? "zero"
                          : boundary == LowpassBoundary::Wrap      ? "wrap"
                                                                   : nullptr;
  const auto* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!id || !extension || !suffix || axis < 0 || axis > 7 || radius < 1 ||
      radius > 4096 || !parameter(cutoff_or_sigma) ||
      (!gaussian && cutoff_or_sigma >= .5) ||
      (kaiser && !parameter(beta, true)))
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               "invalid uniform lowpass parameters",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  WorkflowNode node{id,
                    std::string("curve.lowpass_uniform_") + kernel + suffix,
                    {std::move(input)},
                    {{"axis", axis},
                     {"radius", radius},
                     {"boundary", std::string(extension)},
                     {gaussian ? "sigma" : "cutoff", cutoff_or_sigma}}};
  if (kaiser)
    node.parameters["beta"] = beta;
  return Result<WorkflowNode>(std::move(node));
}
}  // namespace lowpass_detail
/** @brief Filters a sampled signal with a centered Hann-windowed sinc.
 * Input Float32/64 rank 1..8, positive count<=2^40; output values retains shape
 * and dtype with generic facets. axis selects independent signals. Required
 * radius=1..4096 and cutoff in (0,.5) cycles/sample define the exact full
 * kernel. boundary defaults Reflect, period 2*(N-1) without repeated endpoints
 * (N=1 maps to zero); Replicate clamps, Wrap uses Euclidean modulo, Zero adds
 * +0.
 *
 * All five uniform helpers share this contract. Gaussian replaces cutoff with
 * sigma>0 in samples; Kaiser also requires beta>=0. No unused kernel parameter,
 * output conversion, implicit resampling or antialias quality is introduced.
 * Helpers own metadata, are pure/thread-safe, and may throw bad_alloc. Invalid
 * parameters return InvalidArgument/InvalidDomain; Compiler checks actual input
 * dtype/shape/axis and backend availability. No source payload is read at
 * authoring.
 *
 * Runtime rounds the exact normalized whole sum once. Coefficients are
 * certified real enclosures, never pre-rounded Float64 weights. Current
 * accelerated keys report strict scalar fallback. Logical zero taps are
 * omitted; every nonzero tap remains a dependency, including Gaussian
 * underflow. NaN propagation follows first logical tap from -R to R, preserving
 * payload/sign and quieting sNaN. Signed infinite contributions of both signs
 * produce canonical positive qNaN; one sign gives that infinity. Numeric
 * overflow is successful IEEE infinity. All-identical finite extended samples
 * retain their bits, including -0. Other exact zero is +0; nonzero underflow
 * keeps its sign. Caller fenv is preserved.
 *
 * Empty reads no payload. Exact mapped Data support and typed/upstream
 * Validation control dirty mapping; arbitrary legal strides and packed partial
 * outputs work. Source failures retain their provenance. Output/storage
 * metadata outlive the context. Coefficients, limbs, tap maps and outputs
 * consume host capacity/work; cancellation is polled throughout. An unresolved
 * normalizer or final rounding returns ResourceExhausted; no unconverged
 * approximation is published. Finite radius has transition/stopband leakage;
 * choose and verify downsampling quality separately using the public examples.
 */
inline Result<WorkflowNode> lowpass_uniform_hann_sinc_node(
    std::uint64_t id, WorkflowInput input, std::int64_t axis,
    std::int64_t radius, double cutoff,
    LowpassBoundary boundary = LowpassBoundary::Reflect,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return lowpass_detail::uniform(id, "hann_sinc", std::move(input), axis,
                                 radius, cutoff, 0, boundary, profile, false,
                                 false);
}
/** @brief Hamming-windowed sinc; shares the uniform lowpass contract. */
inline Result<WorkflowNode> lowpass_uniform_hamming_sinc_node(
    std::uint64_t id, WorkflowInput input, std::int64_t axis,
    std::int64_t radius, double cutoff,
    LowpassBoundary boundary = LowpassBoundary::Reflect,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return lowpass_detail::uniform(id, "hamming_sinc", std::move(input), axis,
                                 radius, cutoff, 0, boundary, profile, false,
                                 false);
}
/** @brief Blackman-windowed sinc; shares the uniform lowpass contract. */
inline Result<WorkflowNode> lowpass_uniform_blackman_sinc_node(
    std::uint64_t id, WorkflowInput input, std::int64_t axis,
    std::int64_t radius, double cutoff,
    LowpassBoundary boundary = LowpassBoundary::Reflect,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return lowpass_detail::uniform(id, "blackman_sinc", std::move(input), axis,
                                 radius, cutoff, 0, boundary, profile, false,
                                 false);
}
/** @brief Kaiser-windowed sinc with explicit beta; beta=0 is rectangular. */
inline Result<WorkflowNode> lowpass_uniform_kaiser_sinc_node(
    std::uint64_t id, WorkflowInput input, std::int64_t axis,
    std::int64_t radius, double cutoff, double beta,
    LowpassBoundary boundary = LowpassBoundary::Reflect,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return lowpass_detail::uniform(id, "kaiser_sinc", std::move(input), axis,
                                 radius, cutoff, beta, boundary, profile, false,
                                 true);
}
/** @brief Truncated Gaussian with explicit sigma in samples; no cutoff. */
inline Result<WorkflowNode> lowpass_uniform_gaussian_node(
    std::uint64_t id, WorkflowInput input, std::int64_t axis,
    std::int64_t radius, double sigma,
    LowpassBoundary boundary = LowpassBoundary::Reflect,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return lowpass_detail::uniform(id, "gaussian", std::move(input), axis, radius,
                                 sigma, 0, boundary, profile, true, false);
}
namespace lowpass_detail {
inline Result<WorkflowNode> nonuniform(std::uint64_t id, const char* kernel,
                                       WorkflowInput positions,
                                       WorkflowInput values, std::int64_t axis,
                                       double radius, double cutoff_or_sigma,
                                       double beta, LowpassBoundary boundary,
                                       CpuNumericProfile profile, bool gaussian,
                                       bool kaiser) {
  const auto* extension = boundary == LowpassBoundary::Reflect     ? "reflect"
                          : boundary == LowpassBoundary::Replicate ? "replicate"
                          : boundary == LowpassBoundary::Zero      ? "zero"
                          : boundary == LowpassBoundary::Wrap      ? "wrap"
                                                                   : nullptr;
  const auto* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!id || !extension || !suffix || axis < 0 || axis > 7 ||
      !parameter(radius) || !parameter(cutoff_or_sigma) ||
      (kaiser && !parameter(beta, true)))
    return Result<WorkflowNode>(
        Status{ErrorCode::InvalidArgument,
               "invalid nonuniform lowpass parameters",
               FailureReason::InvalidDomain,
               {FailureOrigin::Schema, FailureScope::Unspecified}});
  WorkflowNode node{id,
                    std::string("curve.lowpass_nonuniform_") + kernel + suffix,
                    {std::move(positions), std::move(values)},
                    {{"axis", axis},
                     {"support_radius", radius},
                     {"boundary", std::string(extension)},
                     {gaussian ? "sigma" : "cutoff", cutoff_or_sigma}}};
  if (kaiser)
    node.parameters["beta"] = beta;
  return Result<WorkflowNode>(std::move(node));
}
}  // namespace lowpass_detail
/** @brief Convolves the continuous piecewise-linear signal with a Hann sinc.
 * positions[K] and values independently accept Float32/64; positions is finite
 * strictly increasing, K=2..1048576. values rank 1..8/count<=2^40, with the
 * selected axis of length K; other dimensions are independent signals.
 * Output samples retains values shape/dtype with generic facets at the same
 * original positions. support_radius>0 uses coordinate units; cutoff>0 is in
 * cycles per coordinate unit, with no 0.5 upper bound. Gaussian uses sigma>0
 * in coordinate units instead; Kaiser additionally requires beta>=0.
 *
 * All five nonuniform helpers share this contract. Authoring is
 * pure/thread-safe with owned metadata, InvalidArgument/InvalidDomain for
 * invalid parameters and possible bad_alloc; Compiler checks actual connected
 * metadata/profile. Default Reflect folds continuously at the endpoints;
 * Replicate extends endpoint values, Zero extends +0, and Wrap uses the domain
 * length with a permitted seam jump and no invented connecting segment. No
 * sample-rate inference is performed.
 *
 * Runtime integrates the exact continuous affine reconstruction against the
 * real kernel and divides by its full integral, rounding once. Current
 * accelerated keys report strict fallback. Exact symmetry handles
 * affine/constant/halfway cases; other results use certified polynomial moments
 * and explicit remainder bounds. All participating finite constant bits are
 * retained, including -0; other exact zero is +0, and nonzero underflow keeps
 * its sign. Unlike uniform filtering, nonfinite demanded samples fail
 * OperationFailed/InvalidDomain; final overflow fails ArithmeticOverflow.
 * Caller floating state is preserved.
 *
 * Nonempty requests validate positions globally, then only endpoints of
 * segments intersecting positive integration length for requested signals.
 * Isolated contacts add no reads; coefficient cancellation cannot remove
 * validation. Position changes invalidate all dependent outputs, values follow
 * exact mapped segment support. Typed/upstream closure and errors remain
 * observable. Empty reads no payload. Arbitrary legal strides and owned packed
 * partial outputs survive context teardown. All piece maps, owners, limbs and
 * polynomial storage consume host resources; cancellation is polled through
 * partition/refinement. Huge period counts or unresolved precision/order fail
 * ResourceExhausted, with no approximate substitute.
 * Reconstruction/filter/resampling quality must be measured for the caller's
 * downsampling task; there is no universal Nyquist inferred from nonuniform
 * positions or promise of zero aliasing.
 */
inline Result<WorkflowNode> lowpass_nonuniform_hann_sinc_node(
    std::uint64_t id, WorkflowInput positions, WorkflowInput values,
    std::int64_t axis, double support_radius, double cutoff,
    LowpassBoundary boundary = LowpassBoundary::Reflect,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return lowpass_detail::nonuniform(id, "hann_sinc", std::move(positions),
                                    std::move(values), axis, support_radius,
                                    cutoff, 0, boundary, profile, false, false);
}
/** @brief Hamming sinc continuous convolution; shared nonuniform contract. */
inline Result<WorkflowNode> lowpass_nonuniform_hamming_sinc_node(
    std::uint64_t id, WorkflowInput positions, WorkflowInput values,
    std::int64_t axis, double support_radius, double cutoff,
    LowpassBoundary boundary = LowpassBoundary::Reflect,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return lowpass_detail::nonuniform(id, "hamming_sinc", std::move(positions),
                                    std::move(values), axis, support_radius,
                                    cutoff, 0, boundary, profile, false, false);
}
/** @brief Blackman sinc continuous convolution; shared nonuniform contract. */
inline Result<WorkflowNode> lowpass_nonuniform_blackman_sinc_node(
    std::uint64_t id, WorkflowInput positions, WorkflowInput values,
    std::int64_t axis, double support_radius, double cutoff,
    LowpassBoundary boundary = LowpassBoundary::Reflect,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return lowpass_detail::nonuniform(id, "blackman_sinc", std::move(positions),
                                    std::move(values), axis, support_radius,
                                    cutoff, 0, boundary, profile, false, false);
}
/** @brief Kaiser sinc continuous convolution with explicit beta>=0. */
inline Result<WorkflowNode> lowpass_nonuniform_kaiser_sinc_node(
    std::uint64_t id, WorkflowInput positions, WorkflowInput values,
    std::int64_t axis, double support_radius, double cutoff, double beta,
    LowpassBoundary boundary = LowpassBoundary::Reflect,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return lowpass_detail::nonuniform(
      id, "kaiser_sinc", std::move(positions), std::move(values), axis,
      support_radius, cutoff, beta, boundary, profile, false, true);
}
/** @brief Continuous truncated Gaussian with explicit coordinate-unit sigma. */
inline Result<WorkflowNode> lowpass_nonuniform_gaussian_node(
    std::uint64_t id, WorkflowInput positions, WorkflowInput values,
    std::int64_t axis, double support_radius, double sigma,
    LowpassBoundary boundary = LowpassBoundary::Reflect,
    CpuNumericProfile profile = CpuNumericProfile::Strict) {
  return lowpass_detail::nonuniform(id, "gaussian", std::move(positions),
                                    std::move(values), axis, support_radius,
                                    sigma, 0, boundary, profile, true, false);
}
}  // namespace ps::numeric
