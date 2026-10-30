---
spec_schema_version: 1
id: FRQ-07B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-07
function: circular_fft_convolution
proposed_operation_keys:
- frequency.circular_fft_convolution
numeric_reference: F
oracle_scope: mathematical_reference
research_sources:
- S11
- S09
---

# FRQ-07B: circular_fft_convolution

Fixed periodic circular convolution .

Inherits the [FRQ-07 family contract](FRQ-07_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical and accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family's local F profile takes precedence over inherited strict rounding and accelerated-error limits; structural requirements remain applicable.

## Interface

Operation key: `frequency.circular_fft_convolution`.

### Ports

`input,kernel` → `output` with the input shape.

### Parameters

Axes and anchor are explicit. The period is fixed to H/W and must not be inferred from the kernel. Normalization is none|sum|l1; bias is 0.

NaN, infinities, signed zero, underflow and overflow follow the native arithmetic and floating-point environment; no finite-only scan or numerical-domain failure applies. All listed parameters are mandatory; constants fixed by a named mathematical profile are not parameters. Real parameters denote their stored Float64 values.

## Mathematics

Numeric reference: **F**, as defined by the [FRQ-07 family contract](FRQ-07_contract.md). The mathematical reference remains direct convolution.

The mathematical target is `sum_{j,i} K[j,i]*input[(y+ay-j)%H,(x+ax-i)%W]/N`. The period remains H/W. When the kernel exceeds the period, every tap contributes through modulo indexing or folding with ordinary working-dtype accumulation; no repeated tap may be discarded.

Kernel normalization is evaluated in binary64 before conversion to the native working dtype. Float64 execution is never narrowed to Float32. Float32-input execution may use binary64 arithmetic to retain the Float64 kernel's range, with final conversion to Float32. Explicit FMA is permitted. Correct rounding, fixed ULP bounds and cross-platform bit patterns are not promised. Document absolute/relative error criteria; validate shapes, anchors, boundaries and support independently.

## Behavior

Each output depends on the complete input plane and kernel.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

For length-2 input [1,2] and kernel [1,2,3] with anchor 0, output is [8,10]. This differs from constant-boundary linear convolution. Larger kernels fold repeated taps.

## References

[S11 · SciPy fftconvolve](../research-sources.md#s11) ; [S09 · FFTW: DFT definition](../research-sources.md#s09)
