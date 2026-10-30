---
spec_schema_version: 1
id: RES-08A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-08
function: wiener_nsr
proposed_operation_keys:
- restoration.wiener_nsr
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-08A: wiener_nsr

Scalar-NSR Wiener restoration.

Inherits the [RES-08 family contract](RES-08_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native fp32/fp64 arithmetic follows the RES native floating-point contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

Operation key: `restoration.wiener_nsr`.

### Ports

`observation,psf` → `estimate` with the original shape and dtype.

### Parameters

Specify axis, anchor, nsr≥0 (dimensionless noise-to-signal power ratio), and the singular policy explicitly.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

`x=IFFT(conj(H)*FFT(y)/(|H|²+nsr))` is evaluated as native FFT, pointwise solve and inverse FFT stages. NSR is not lambda multiplied by a regularizer frequency response. The H=0 case must still be handled when nsr=0.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Each output plane depends on the complete input plane and the complete PSF.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

A delta PSF yields y/(1+nsr). With nsr=0, a delta PSF preserves valid finite inputs without a numerical-validation pass. At a zero frequency, singular=zero and singular=error differ as specified.

## References

[S22 · scikit-image restoration](../research-sources.md#s22);[S09 · FFTW: DFT definition](../research-sources.md#s09)
