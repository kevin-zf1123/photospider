#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "photospider/ops/frequency.hpp"
#include "photospider/ops/tensor_description.hpp"

namespace ps::restoration {
/** @brief Append RES-03A for a complete FRQ10 SDK directory transactionally.
 * thresholds is a Float64 [options.levels,3] Result, ordered LH_y, HL_x, HH.
 * mode is hard/soft, explicit; finite nonnegative thresholds representable in
 * the working dtype are unchecked caller premises. Hard retains equality;
 * soft clears equality to +0. Zero thresholds and LL preserve every input bit.
 * All metadata is retained; LL reuses its reference without an arithmetic node.
 * Positive levels are required. Deep directories use ordinary <=5-level nodes.
 * Shape/profile/directory errors return InvalidArgument/TypeMismatch, with no
 * document change; bad_alloc may escape. References own no payload/cache.
 * Serialize edits to one document; independent documents are safe. Execution
 * propagates upstream, resource and cancellation failures; immutable Result
 * owners/read windows can outlive their ExecutionContext. CPU native fp32/fp64
 * follows RES_native_contract, without exact-expression or ULP guarantees.
 */
PHOTOSPIDER_OPS_API Result<frequency::WaveletBands> threshold_wavelet_details(
    WorkflowDocument&, const frequency::WaveletBands&, WorkflowInput thresholds,
    const std::string& mode);

/** @brief Append RES-03B as explicit wrap permutation, FRQ10 analysis,
 * RES03A thresholding, FRQ10 inverse, inverse permutation, native sum/division.
 * options.profile is haar_mean_lifting_v1/cdf53_float_lifting_v1; all fields
 * are explicit. shifts is a nonempty list of signed (y,x) offsets, unique
 * modulo original H/W by caller premise. S_s(input)[y,x]=input[y+sy,x+sx]
 * modulo H/W; wavelet boundaries retain their named nonperiodic profile.
 * The mean uses input-dtype state in list order and one final native division.
 * levels=0 returns input unchanged and appends no nodes or threshold reads.
 * Threshold, error, lifecycle, thread and cache rules match the helper above.
 * The document contains ordinary nodes only, with no cycle-spin native key.
 */
PHOTOSPIDER_OPS_API Result<WorkflowInput> cycle_spin_wavelet_denoise(
    WorkflowDocument&, WorkflowInput input, WorkflowInput thresholds,
    const frequency::WaveletOptions& options,
    const std::vector<std::array<std::int64_t, 2>>& shifts,
    const std::string& mode);

/** @brief Explicit configuration of the native haar_opponent_v1 profile.
 * block_size is 1,2,4,8,16 or 32; group_size is a power of two in 1..64.
 * reference_step is in 1..block_size, search_radius is nonnegative.
 * y_axis/x_axis select the two image axes. All fields are required.
 * match_hard/match_wiener are strict upper bounds on mean squared patch
 * distance, in squared input units. hard_threshold multiplies sigma.
 * kaiser_beta controls the separable aggregation window (zero is uniform).
 * Finite, nonnegative thresholds/beta/sigma, representable parameters and
 * finite participating values/intermediates are unchecked caller premises.
 *
 * Reference centers visit 0, step, ... and the final pixel in y,x order.
 * A block starts at center-floor(block_size/2). reflect101 extends the whole
 * image; a singleton axis repeats. Search includes every integer center in
 * the radius square. The reference is first; other accepted centers sort by
 * (native squared distance, y, x). The closest power-of-two prefix, bounded
 * by group_size, forms a group. Spatial and group transforms are orthonormal
 * recursive Haar. Hard threshold retains equality; weights are inverse
 * nonzero counts, or one when zero. Wiener gain is b*b/(b*b+sigma*sigma);
 * its weight is inverse sum(gain*gain), or one when zero. Kaiser-weighted
 * reconstructed blocks accumulate on the original image and are normalized.
 * The shared sigma^-2 aggregation factor cancels and is omitted.
 */
struct Bm3dOptions {
  std::int64_t y_axis, x_axis;
  std::string profile;
  std::int64_t block_size, search_radius, group_size, reference_step;
  double match_hard, match_wiener, hard_threshold, kaiser_beta;
};

/** @brief Append RES-04A, key restoration.bm3d_native, transactionally.
 * input is one nonempty rank-two Float32/Float64 plane; sigma is Float64 [1].
 * output has the same shape/dtype, with native intermediates throughout the
 * explicit hard-threshold then pilot-guided Wiener stages. sigma=0 copies
 * input bits without either denoising stage or numerical-domain validation.
 * Signed/HDR samples retain their units; no /255, clipping or metadata-based
 * color interpretation occurs. The operation reads the whole logical plane
 * and computes once for the requested output tiles/ROI.
 *
 * Invalid static structure/profile returns InvalidArgument/TypeMismatch;
 * capacity/work failures return ResourceExhausted and cancellation propagates.
 * The helper owns no payload and changes the document only after successful
 * parameter encoding and ID allocation; bad_alloc may escape. Serialize edits
 * to one document. Independent documents/executions are safe; prepared state
 * is immutable, with invocation-local scratch and no input-dependent cache.
 * Returned Results retain ordinary kernel ownership and lifetime rules.
 */
PHOTOSPIDER_OPS_API Result<WorkflowInput> bm3d_native(
    WorkflowDocument&, WorkflowInput input, WorkflowInput sigma,
    const Bm3dOptions& options);

/** @brief Append RES-04B, key restoration.cbm3d_native.
 * input is rank three with two image axes and exactly three RGB components.
 * An explicit FMT description/group must identify opaque native RGB, its
 * primaries, transfer and scene/display reference. Alpha and AOVs are rejected.
 * Output storage/order and source color interpretation are preserved; Results
 * themselves carry no color description. No transfer conversion is performed.
 * The fixed orthonormal opponent coordinates are (R+G+B)/sqrt(3),
 * (R-B)/sqrt(2), (R-2G+B)/sqrt(6). The first coordinate supplies shared
 * block matching, with matching thresholds in its squared coordinate units;
 * all three channels filter and weight independently with the
 * same sigma, followed by the transpose inverse. sigma is isotropic RGB noise
 * in the input coordinate units. Other behavior matches bm3d_native.
 */
PHOTOSPIDER_OPS_API Result<WorkflowInput> cbm3d_native(
    WorkflowDocument&, WorkflowInput input, WorkflowInput sigma,
    const Bm3dOptions& options, const TensorDescription& input_description,
    const std::string& group);

}  // namespace ps::restoration
