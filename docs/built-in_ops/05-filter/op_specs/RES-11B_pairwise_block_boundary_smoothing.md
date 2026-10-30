---
spec_schema_version: 1
id: RES-11B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-11
function: pairwise_block_boundary_smoothing
proposed_operation_keys:
- restoration.pairwise_block_boundary_smoothing
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-11B: pairwise_block_boundary_smoothing

Pairwise smoothing across known block boundaries.

Inherits the [RES-11 family contract](RES-11_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native arithmetic follows RES_native_contract without NUM exact-expression or four-ULP requirements.

## Interface

Operation key: `restoration.pairwise_block_boundary_smoothing`.

### Ports

`input, application_mask` → same-shape `output`; the mask is finite and in [0,1], with any component broadcasting declared explicitly.

### Parameters

block_width and block_height ≥ 2; integer origin_x and origin_y; threshold τ > 0; strength ∈ [0, 1/2] (0.25 is illustrative, not a default); axes; fixed execution order: horizontal x pass, then vertical y pass.

Use native IEEE arithmetic under RES_native_contract; no NUM whole-expression rounding or NaN payload priority is required. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematics

Numeric classification: **F**, native fp32/fp64 under [RES_native_contract](RES_native_contract.md).

For each adjacent pair (l,r) on a block boundary, provided both lie inside the image, if abs(r-l) ≤ τ then set delta = strength*min(Ml,Mr)*(r-l), evaluated in native precision, and produce l+=RN_t(l+delta), r-=RN_t(r-delta); otherwise copy both values. Pairs within a pass do not overlap because each block dimension is at least 2. The second, y pass reads the rounded output of the x pass. All other samples are bitwise copies. If min(Ml,Mr)=0 or strength=0, copy both values without computing the difference. A zero mask at either endpoint suppresses the whole pair, preserving every unmasked sample bit-for-bit.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

The support is the union of the two fixed passes, with a maximum halo of one per axis (a corner may follow a two-pass dependency). This is not global deblocking or codec dequantization.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

For the two-pixel input [0,1], block_width=2, origin_x=1, strength=0.25, and τ=1, the output is [0.25,0.75]. Equality at τ activates correction; a zero mask preserves both samples; shifting the block origin changes which edge is selected.

## References

[S22 · scikit-image restoration](../research-sources.md#s22)
