#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/ops/numeric/diagnostics.hpp"

namespace ps::numeric {
/** @brief Explicit discrete/continuous boundary extension. */
enum class LowpassBoundary { Reflect, Replicate, Zero, Wrap };
namespace lowpass_detail {
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
      radius > 4096)
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
 * A single-tensor Result is the input; output values retains its full shape and
 * dtype with ordinary data. axis selects independent signals. Required
 * radius=1..4096 and cutoff in (0,.5) cycles/sample define the exact full
 * kernel. boundary defaults Reflect, period 2*(N-1) without repeated endpoints
 * (N=1 maps to zero); Replicate clamps, Wrap uses Euclidean modulo, Zero adds
 * +0.
 *
 * All five uniform helpers share this contract. Gaussian replaces cutoff with
 * sigma>0 in samples; Kaiser also requires beta>=0. No unused kernel parameter,
 * output conversion, implicit resampling or antialias quality is introduced.
 * Helpers own metadata, are pure/thread-safe, and may throw bad_alloc. Invalid
 * IDs, enum selections, axes and structural radius return
 * InvalidArgument/InvalidDomain; Compiler checks actual input dtype/shape/axis
 * and backend availability. Finite cutoff/sigma/beta domains are unchecked
 * caller premises, including during authoring. No source payload is read.
 *
 * Strict rounds the exact normalized whole sum once. Accelerated follows
 * CpuNumericProfile's final FP32 bound, reusing certified coefficient
 * enclosures and propagating convolution error. Unresolved cases use strict
 * fallback; rounded weights alone do not define the reference. Logical zero
 * taps are omitted; every nonzero tap remains a dependency, including Gaussian
 * underflow. NaN propagation follows first logical tap from -R to R, preserving
 * payload/sign and quieting sNaN. Signed infinite contributions of both signs
 * produce canonical positive qNaN; one sign gives that infinity. Numeric
 * overflow is successful IEEE infinity. All-identical finite extended samples
 * retain their bits, including -0. Other exact zero is +0; nonzero underflow
 * keeps its sign. Caller fenv is preserved.
 *
 * `input` is a Result with exactly one tensor member under any
 * structurally valid schema id/version/member key. Use its complete
 * `sample_shape()`, including batch axes: Float32/64, rank 1..8, positive
 * extents and logical count <=2^40. Output port `values` is an immutable
 * `photospider.tensor` v1/member `samples` Result with the same shape/dtype and
 * ordinary data. Results retain their sample storage owners.
 *
 * Each selected output tile uses one fixed CPU task. Reads cover exactly the
 * nonzero taps after boundary mapping, closed on the source tile grid. Extra
 * physical samples are not numerical operands. Output shape uses ordinary
 * axes, with no input batch/atomic grouping. Only selected outputs are
 * allocated. Empty selection emits no task after static preparation. Affine
 * source views are borrowed; output storage owners survive context teardown.
 * Each invocation owns preallocated math state, 8*(2R+1) sample bytes and,
 * for accelerated profiles, 16*(R+1) coefficient bytes. Prepared state is
 * immutable; concurrent Runs own separate output/scratch. Cancellation and
 * resource failures return directly. A call publishes after all writes succeed
 * and retire. An unresolved normalizer or final rounding returns
 * ResourceExhausted; no unconverged
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
  if (!id || !extension || !suffix || axis < 0 || axis > 7)
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
 * `positions` and `values` are Results with one tensor member
 * under any structurally valid schema id/version/member key. Use complete
 * `sample_shape()` values, including batch axes. `positions` is [K], finite
 * strictly increasing Float32/64, K=2..1048576. `values` independently accepts
 * Float32/64, rank 1..8/count<=2^40, with the selected axis of length K; other
 * dimensions are independent signals. Output `samples` is a
 * `photospider.tensor` v1/member `samples` Result preserving full values
 * shape/dtype with ordinary data at the original positions. Results
 * retain their sample storage owners.
 * support_radius>0 uses coordinate units; cutoff>0 is in
 * cycles per coordinate unit, with no 0.5 upper bound. Gaussian uses sigma>0
 * in coordinate units instead; Kaiser additionally requires beta>=0.
 *
 * All five nonuniform helpers share this contract. Authoring is
 * pure/thread-safe with owned metadata, InvalidArgument/InvalidDomain for
 * invalid IDs, axes or enum selections, and possible bad_alloc. Compiler checks
 * connected metadata and profile. Finite support_radius/cutoff/sigma/beta
 * domains are unchecked caller premises, including during authoring. Default
 * Reflect folds at the endpoints; Replicate extends endpoint values, Zero
 * extends +0, and Wrap uses the domain length with a permitted seam jump and no
 * invented connecting segment. No sample-rate inference is performed.
 *
 * Runtime integrates the exact continuous affine reconstruction against the
 * real kernel and divides by its full integral, rounding once. Current
 * accelerated keys use strict fallback. Exact symmetry handles
 * affine/constant/halfway cases; other results use certified polynomial moments
 * and explicit remainder bounds. All participating finite constant bits are
 * retained, including -0; other exact zero is +0, and nonzero underflow keeps
 * its sign. Finite strictly increasing positions, finite contributing endpoint
 * values and finite representable selected outputs are caller preconditions.
 * Caller floating state is preserved.
 *
 * Each selected output tile uses one ABI 8 CPU task. Reads cover complete
 * positions and the full filtered values axis for selected other-axis
 * coordinates, closed on source grids. The kernel searches affine views
 * directly and streams positive-length segments using exact integer geometry.
 * It retains two pieces and fixed arithmetic/polynomial scratch; source
 * coordinates and partitions are not copied into growing containers. Output
 * uses ordinary axes and zero atomic trailing axes. Only selected tiles are
 * allocated. Empty selection emits no task after static preparation.
 *
 * Immutable preparation supports concurrent Runs with independent scratch.
 * Cancellation/resource failures return before shortcuts or precision retries.
 * The call publishes after all declared writes succeed and retire; escaped
 * output storage remains valid beyond context lifetime. Precision 128..4096
 * and Taylor order <=512 bound refinement. Uncertified rounding/capacity
 * returns ResourceExhausted. Large support can traverse many periods with
 * fixed storage; each piece and arithmetic loop polls cancellation.
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
