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

/** @brief Explicit native Chambolle-Pock parameters for RES-05.
 * All fields are required. y_axis and x_axis are distinct sample-shape axes
 * in a rank 2..8 Float32/Float64 tensor; other axes are independent planes.
 * max_iterations is nonnegative. Finite representable lambda>=0, tau>0,
 * sigma>0, theta in [0,1], dx>0, dy>0 and
 * tau*sigma*4*(1/(dx*dx)+1/(dy*dy))<1 are unchecked numerical premises.
 * The objective is 0.5*sum((u-input)^2)+lambda*sum(phi(Ku)), with no-flux
 * forward differences and their algebraic transpose. State is native input
 * precision. Every complete step stores dual, primal and extrapolated state.
 */
struct TvRofOptions {
  std::int64_t y_axis, x_axis;
  double lambda;
  std::int64_t max_iterations;
  double tau, sigma, theta, dx, dy;
};

/** @brief Explicit controls for the independent RES-05 early-stop members.
 * check_interval is a positive Int64 completed-round count. Finite nonnegative
 * atol is in average-objective units; rtol is dimensionless. The coherent
 * per-plane primal-dual gap must pass in every plane. Nonfinite checks cannot
 * pass. There is at most one asynchronous check; busy triggers are discarded.
 * A winning check returns its checked state; reaching max_iterations commits
 * the final state without waiting for a pending reduction to finish. Worker
 * cleanup is bounded by cancellation checkpoints before scratch is released.
 * Returned rounds may vary between executions. There are no diagnostic ports.
 */
struct TvStoppingOptions {
  std::int64_t check_interval;
  double atol, rtol;
};

/** @brief Append RES-05A, key restoration.tv_rof_isotropic.
 * phi(gx,gy)=hypot(gx,gy); the anisotropic member uses abs(gx)+abs(gy).
 * The output retains the input shape/dtype. No color/alpha interpretation,
 * clipping or implicit normalization occurs. lambda=0 or max_iterations=0
 * copies input bits and reads only requested samples. Other calls solve whole
 * logical planes once and publish requested tiles/ROI from the global result.
 * Fixed-step members run exactly max_iterations, without data-dependent exit.
 *
 * These helpers append one ordinary node transactionally; they own no payload.
 * Static parameter/schema/axis errors are checked during compilation and
 * preparation. Resource, upstream and cancellation failures propagate.
 * bad_alloc may escape the helper. Serialize mutation of one document;
 * independent documents and executions are safe. Prepared state is immutable;
 * all input-dependent state uses invocation-owned scratch, with no global
 * payload cache. Results retain standard kernel ownership/lifetime.
 */
PHOTOSPIDER_OPS_API Result<WorkflowInput> tv_rof_isotropic(
    WorkflowDocument&, WorkflowInput input, const TvRofOptions& options);

/** @brief Append RES-05B, key restoration.tv_rof_anisotropic.
 * Interface, ownership and errors match tv_rof_isotropic; phi is anisotropic.
 */
PHOTOSPIDER_OPS_API Result<WorkflowInput> tv_rof_anisotropic(
    WorkflowDocument&, WorkflowInput input, const TvRofOptions& options);

/** @brief Append RES-05C, key restoration.tv_rof_isotropic_early_stop.
 * Uses TvStoppingOptions and the isotropic fixed-step recurrence. Identity
 * branches do not check convergence. No round-zero or forced final check.
 * All failures retain failure status after normal-termination races.
 */
PHOTOSPIDER_OPS_API Result<WorkflowInput> tv_rof_isotropic_early_stop(
    WorkflowDocument&, WorkflowInput input, const TvRofOptions& options,
    const TvStoppingOptions& stopping);

/** @brief Append RES-05D, key restoration.tv_rof_anisotropic_early_stop.
 * Uses the anisotropic recurrence and the same stopping/lifecycle contract.
 */
PHOTOSPIDER_OPS_API Result<WorkflowInput> tv_rof_anisotropic_early_stop(
    WorkflowDocument&, WorkflowInput input, const TvRofOptions& options,
    const TvStoppingOptions& stopping);

}  // namespace ps::restoration
