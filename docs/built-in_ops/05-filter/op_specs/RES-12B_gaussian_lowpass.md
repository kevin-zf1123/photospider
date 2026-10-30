---
spec_schema_version: 1
id: RES-12B
kind: authoring_helper
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-12
function: gaussian_lowpass
proposed_operation_keys: []
numeric_reference: F
oracle_scope: composed_math_references
---

# RES-12B: gaussian_lowpass

Explicit low-pass baseline for screen-pattern suppression.

Inherits the [RES-12 family contract](RES-12_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native arithmetic follows RES_native_contract without NUM exact-expression or four-ULP requirements.

## Interface

### Ports

`input` → `output`.

### Parameters

All FIL-04B parameters are explicit. No default resizing or automatic frequency estimation is used; axes are explicit.

Use native IEEE arithmetic under RES_native_contract; no NUM whole-expression rounding or NaN payload priority is required. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematics

Numeric classification: **F**, native fp32/fp64 under [RES_native_contract](RES_native_contract.md).

Reuse the gaussian_baked64_v1 low-pass filter as a named authoring preset, not as a new mathematical primitive. This delegation preserves the existing FIL coefficient profile and interface accuracy; it does not impose NUM strict-expression or four-ULP requirements on new RES-native kernels, and does not silently redefine FIL arithmetic. For downsampling, connect an explicit sampling node from the geometry category; this filter alone does not recover detail hidden by the screen pattern.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Local Gaussian actual support; no whole-image spectral read.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

The periodic checker pattern is attenuated, while genuine high-frequency texture is also affected.

## References

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S26 · scikit-image filters](../research-sources.md#s26)
