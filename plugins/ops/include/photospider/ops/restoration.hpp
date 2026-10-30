#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "photospider/ops/frequency.hpp"

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
}  // namespace ps::restoration
