---
spec_schema_version: 1
id: FRQ-06A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-06
function: frequency_multiply
proposed_operation_keys:
- frequency.frequency_multiply_strict
- frequency.frequency_multiply_accelerated_apple_silicon
- frequency.frequency_multiply_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S09
---

# FRQ-06A: frequency_multiply

Pointwise complex-frequency multiplication.

Inherits the [FRQ-06 family contract](FRQ-06_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical and accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Interface

Operation keys: `frequency.frequency_multiply_strict`, `frequency.frequency_multiply_accelerated_apple_silicon`, `frequency.frequency_multiply_accelerated_x86_64`.

### Ports

`f_real,f_imag,h_real,h_imag` → `out_real,out_imag`; F dtype is Float32/Float64 and H may be Float64. Packing, full/half representation, index order, and original grid must be compatible.

### Parameters

`broadcast_axes` is explicit and may not broadcast across frequency axes. The response tag must be dimensionless. Reject F and H with different `index_order` values.

NaN, infinities and floating-point overflow follow the corresponding NUM operation; no finite-only input rule applies and overflow is not an operation failure. All listed parameters are mandatory; constants fixed by a named mathematical profile are not parameters. Real parameters denote their stored Float64 values.

## Mathematics

Numeric reference: **E**; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

`Re=RN_t(fr*hr-fi*hi)`, `Im=RN_t(fr*hi+fi*hr)`. Each component evaluates its exact products and sum with one final RN; this is not three rounded complex-multiply operations. Real-only output still needs fi/hi because of the cross terms.

## Behavior

Output samples depend pointwise on the corresponding samples of F and H. Static shape, transformed-axis, packing and index-order compatibility is required; sampling-unit and numerical association premises remain caller obligations.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Fixture: (1+2i)*(3+4i)=-5+10i; also cover zero imaginary parts and signed zero. Incompatible static original shapes are rejected at construction.

## References

[S09 · FFTW: DFT definition](../research-sources.md#s09)
