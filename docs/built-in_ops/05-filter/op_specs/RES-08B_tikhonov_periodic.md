---
spec_schema_version: 1
id: RES-08B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-08
function: tikhonov_periodic
proposed_operation_keys:
- restoration.tikhonov_periodic
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-08B: tikhonov_periodic

Periodic Tikhonov restoration.

Inherits the [RES-08 family contract](RES-08_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native fp32/fp64 arithmetic follows the RES native floating-point contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Interface

Operation key: `restoration.tikhonov_periodic`.

### Ports

`observation,psf` → `estimate`.

### Parameters

Explicitly supply λ≥0, `regularizer=identity|gradient|laplacian`, dx/dy>0, `singular=error|zero`, axis, and anchor.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Finite-parameter, control-model and numerical-domain premises are unchecked caller obligations. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematics

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

Minimize `||Ax-y||²+λ||D x||²`. Let R=|D|²: identity gives 1; gradient gives `4sin²(πkx/W)/dx²+4sin²(πky/H)/dy²`; Laplacian is the square of that gradient expression. `x=IFFT(conj(H)*FFT(y)/(|H|²+λR))` uses native FFT, pointwise solve and inverse FFT stages. Gradient and Laplacian do not penalize DC; R itself is not the signed operator D.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Each output plane depends on the complete input plane and PSF. The regularizer derives from static parameters and the grid, not image samples.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Delta PSF with λ=0 yields y. A gradient regularizer preserves a constant image (the PSF has unit DC). Identity regularization with λ=1 and delta PSF yields y/2. Apply the singular policy when H and D are both zero.

## References

[S22 · scikit-image restoration](../research-sources.md#s22);[S09 · FFTW: DFT definition](../research-sources.md#s09)
