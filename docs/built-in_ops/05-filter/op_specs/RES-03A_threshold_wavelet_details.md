---
spec_schema_version: 1
id: RES-03A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: RES-03
function: threshold_wavelet_details
proposed_operation_keys:
- restoration.threshold_wavelet_details
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-03A: threshold_wavelet_details

Wavelet detail thresholding.

Inherits the [RES-03 family contract](RES-03_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native fp32/fp64 arithmetic follows the RES native floating-point contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

Operation key: `restoration.threshold_wavelet_details`.

### Ports

`bands` FilterBands/v1 and `thresholds` Float64 `[levels,3]` → new `bands`; LL residual is bit-copied. Per level, order is LH_y, HL_x, HH.

### Parameters

Require an explicit `mode=hard|soft`; every threshold≥0. Under the positive-extent rule, this member does not accept an empty threshold table when levels=0; use the identity-bypass helper without creating this node.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

Hard threshold: abs(d)<T yields +0; otherwise copy d, so equality retains the coefficient. Soft threshold: abs(d)≤T yields +0; otherwise `sign(d)*(abs(d)-T)` in the native dtype. At T=0 all detail coefficients are bit-copied, with no extra finite validation. For nonzero T, NaN coefficients propagate through native arithmetic and infinite coefficients follow the stated arithmetic; they are not rejected by a finite-only rule. LL adds no arithmetic validation.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

The complete static band directory is checked against the profile. Thresholding is pointwise: an output detail coefficient depends only on the matching input coefficient and the threshold cell for its level and orientation. Runtime associations are caller premises. The LL output is a bit copy of the input LL.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

At d=T, hard mode retains the coefficient and soft mode clears it to zero. Preserve the sign of negative details. At T=0, preserve NaN and -0 bits. The inverse may consume the edited bands.

Hard equality, native fp32/fp64 clear/copy boundaries and native control rounding are covered. Arithmetic NaN payload bits are not prescribed; zero-threshold and LL copies retain input bits.

## References

[S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17);[S22 · scikit-image restoration](../research-sources.md#s22)
