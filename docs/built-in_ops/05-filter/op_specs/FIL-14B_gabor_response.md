---
spec_schema_version: 1
id: FIL-14B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-14
function: gabor_response
proposed_operation_keys:
- filter.gabor_response_strict
- filter.gabor_response_accelerated_apple_silicon
- filter.gabor_response_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
- S26
---

# FIL-14B: gabor_response

Gabor correlation response.

Inherits [FIL-14](FIL-14_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.gabor_response_strict`, `filter.gabor_response_accelerated_apple_silicon`, `filter.gabor_response_accelerated_x86_64`.

`input,kernel_real,kernel_imag` → `response_real,response_imag`; kernel arrays have the same shape and are Float64.

### Parameters

Axes and anchor are explicit; boundary=reflect_half, cval=0; normalization=none and bias=0 are fixed. Optional structural `expected_shape` and `expected_dtype` parameters bind the responses to an SDK bank directory.

## Mathematics

Numerical class: **E**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Apply two FIL-01B correlations, each with one exact expression and one final RN_t. Correlation does not conjugate the complex kernel: this is two real-kernel correlations, not a Hermitian inner product.

## Behavior

`response_real` depends only on `kernel_real` and `response_imag` only on `kernel_imag`; the outputs are independently requestable.

## Acceptance

An impulse produces the kernel flipped according to correlation direction. A phase shift of π/2 gives the corresponding sign/swap relation. Test direction with an asymmetric kernel.

## References

[S01 · OpenCV Image Filtering](../research-sources.md#s01)；[S26 · scikit-image filters](../research-sources.md#s26)
