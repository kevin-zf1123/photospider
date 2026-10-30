---
spec_schema_version: 1
id: FRQ-08A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2
implementation_status: implemented
parent_id: FRQ-08
function: overlap_add
proposed_operation_keys:
- frequency.overlap_add
numeric_reference: F
oracle_scope: mathematical_reference
research_sources:
- S12
---

# FRQ-08A: overlap_add

Overlap-add convolution .

Inherits the [FRQ-08 family contract](FRQ-08_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical and accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family's local F profile takes precedence over inherited strict rounding and accelerated-error limits; structural requirements remain applicable.

## Interface

Operation key: `frequency.overlap_add`.

### Ports

`input,kernel` → `output`, using zero extension and full/same/valid shapes as in FIL01A.

### Parameters

Axes, anchor, output shape, and normalization none|sum|l1 are explicit. block_payload_height and block_payload_width are positive. fft_shape is at least payload+kernel-1. The block grid is anchored at the global input origin and does not restart at an ROI. A fixed kernel has no radius field.

NaN, infinities, signed zero, underflow and overflow follow the native arithmetic and floating-point environment; no finite-only scan or numerical-domain failure applies. All listed parameters are mandatory; constants fixed by a named mathematical profile are not parameters. Real parameters denote their stored Float64 values.

## Mathematics

Numeric reference: **F**, as defined by the [FRQ-08 family contract](FRQ-08_contract.md). The mathematical reference remains direct convolution.

The mathematical target is FIL01A zero-extension linear convolution. Each non-overlapping input payload contributes its full convolution; every covering block contributes to each retained output point. Transform outputs and overlap accumulation use the native working dtype. Exact accumulation with one final rounding is not required.

Kernel normalization is evaluated in binary64 before conversion to the native working dtype. Float64 execution is never narrowed to Float32. Float32-input execution may use binary64 arithmetic to retain the Float64 kernel's range, with final conversion to Float32. Explicit FMA is permitted. Correct rounding, fixed ULP bounds and cross-platform bit patterns are not promised. Document absolute/relative error criteria; validate shapes, anchors, boundaries and support independently.

## Behavior

Support is block-conservative: it covers the blocks involved in the requested outputs and the ranges their FFTs read, including gaps and halos; pointwise exact support is not claimed. The full kernel is a control input, and an empty selection reads no block.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Verify F-profile numerical agreement with direct convolution for at least three block shapes, impulses crossing block boundaries, whole-image versus ROI-tile requests and kernels larger than the payload. Check output shapes, anchors and the fixed global block origin exactly.

## References

[S12 · SciPy oaconvolve](../research-sources.md#s12)
