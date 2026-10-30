---
spec_schema_version: 1
id: RES-11A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-11
function: low_contrast_smoothing
proposed_operation_keys:
- restoration.low_contrast_smoothing
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-11A: low_contrast_smoothing

Selective smoothing in low-contrast regions.

Inherits the [RES-11 family contract](RES-11_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native arithmetic follows RES_native_contract without NUM exact-expression or four-ULP requirements.

## Interface

Operation key: `restoration.low_contrast_smoothing`.

### Ports

`input, application_mask` → `output`; the mask has the same shape or uses explicit component broadcasting, and each consumed mask value is finite and in [0,1].

### Parameters

radius ≥ 1; range_threshold τ > 0; strength ∈ [0,1] (0.5 is an illustrative value, not a default); boundary=reflect_half; axes.

Use native IEEE arithmetic under RES_native_contract; no NUM whole-expression rounding or NaN payload priority is required. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematics

Numeric classification: **F**, native fp32/fp64 under [RES_native_contract](RES_native_contract.md).

If M=0 or strength=0, bit-copy the center without reading the neighborhood. Otherwise, compute R=max-min in native precision; a NaN determining R propagates to the output rather than selecting the copy branch. When R≤τ, let B be a native window mean and produce RN_t(I+M*strength*(B-I)); when R>τ, bit-copy I. No random dither is applied; display quantization and dithering require explicit FMT nodes.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

An activated window has a maximum halo of r, and the mask value at the center sample gates it. Range selection uses all window samples; reading only the samples that determine the extrema is insufficient.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

A flat quantization staircase is smoothed when its range is within threshold. A range exactly equal to τ activates smoothing; fine texture with range >τ is retained. M=0 must not read poisoned neighbors.

## References

[S22 · scikit-image restoration](../research-sources.md#s22)
