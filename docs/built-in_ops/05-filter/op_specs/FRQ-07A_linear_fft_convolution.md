---
spec_schema_version: 1
id: FRQ-07A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2
implementation_status: implemented
parent_id: FRQ-07
function: linear_fft_convolution
proposed_operation_keys:
- frequency.linear_fft_convolution
numeric_reference: F
oracle_scope: mathematical_reference
research_sources:
- S11
- S09
---

# FRQ-07A: linear_fft_convolution

Linear convolution family .

Inherits the [FRQ-07 family contract](FRQ-07_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical and accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family's local F profile takes precedence over inherited strict rounding and accelerated-error limits; structural requirements remain applicable.

## Interface

Operation key: `frequency.linear_fft_convolution`.

### Ports

`input,kernel` → `output`; input and output are Float data and the kernel is Float64, as in FIL-01A.

### Parameters

Axes; explicit anchor; shape=same|full|valid; fixed constant-zero boundary; normalization none|sum|l1; bias 0; fft_padding_shape contains two Int64 values, each at least H+Kh-1/W+Kw-1 respectively. The constructor may choose any sizes satisfying these conditions. No implicit periodic folding.

NaN, infinities, signed zero, underflow and overflow follow the native arithmetic and floating-point environment; no finite-only scan or numerical-domain failure applies. All listed parameters are mandatory; constants fixed by a named mathematical profile are not parameters. Real parameters denote their stored Float64 values.

## Mathematics

Numeric reference: **F**, as defined by the [FRQ-07 family contract](FRQ-07_contract.md). The mathematical reference remains direct convolution.

The mathematical target is FIL01A zero-extension convolution with the same anchor, shapes and output origin. Legal FFT padding affects execution and rounding, without changing target support or origin.

Kernel normalization is evaluated in binary64 before conversion to the native working dtype. Float64 execution is never narrowed to Float32. Float32-input execution may use binary64 arithmetic to retain the Float64 kernel's range, with final conversion to Float32. Explicit FMA is permitted. Correct rounding, fixed ULP bounds and cross-platform bit patterns are not promised. Document absolute/relative error criteria; validate shapes, anchors, boundaries and support independently.

## Behavior

Each output depends on the complete input plane and complete kernel.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

For an asymmetric even-sized kernel, verify every supported shape and anchor against FIL01A using documented F-profile error criteria. Different valid padding may change rounding but not support, origin or shape; padding one element too small fails preflight.

## References

[S11 · SciPy fftconvolve](../research-sources.md#s11) ; [S09 · FFTW: DFT definition](../research-sources.md#s09)
