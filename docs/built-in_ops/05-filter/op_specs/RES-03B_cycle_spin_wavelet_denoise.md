---
spec_schema_version: 1
id: RES-03B
kind: authoring_helper
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: RES-03
function: cycle_spin_wavelet_denoise
proposed_operation_keys: []
numeric_reference: F
oracle_scope: composed_math_references
---

# RES-03B: cycle_spin_wavelet_denoise

Wavelet denoising with a fixed shift set.

Inherits the [RES-03 family contract](RES-03_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native fp32/fp64 arithmetic follows the RES native floating-point contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

The SDK helper `ps::restoration::cycle_spin_wavelet_denoise` and the helper primitives `restoration.wrap_shift` and `restoration.accumulate_native` are specified in the [RES-03 interface](RES-03_contract.md#interface).

### Ports

`input,thresholds` → `output` with the original shape. Threshold schema is the same as member A. At levels=0, explicitly bypass input and do not read thresholds.

### Parameters

`profile=haar_mean_lifting_v1|cdf53_float_lifting_v1`; specify levels and a nonempty list of unique integer (y,x) shifts. Callers guarantee uniqueness after reduction modulo H/W; no runtime numerical-domain check is required. The complete shift list is explicit and nonrandom. Boundary analysis follows the profile; do not silently switch to periodic wavelets.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

For each shift s, first apply the wrap permutation S_s to the input; then run FRQ10 analysis → RES-03A thresholding → FRQ10 inverse → S_-s. Compute their mean with native input-dtype accumulation and division; stable or pairwise accumulation is permitted. Average only over the specified shift set; a finite set does not guarantee full translation invariance.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

The result depends on the complete input plane and the complete band collection; shifts and thresholds are control inputs.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

A single shift (0,0) is equivalent to the chain without cycle spinning. T=0 does not guarantee bit identity for arbitrary floating-point analysis/reconstruction. Full-period shift coverage can test translation equivariance, subject to the boundary and algorithm; define the comparison by complete enumeration.

Stage rounding boundaries are not removed by merging stages.

## References

[S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17);[S22 · scikit-image restoration](../research-sources.md#s22)
