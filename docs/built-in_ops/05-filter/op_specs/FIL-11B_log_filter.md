---
spec_schema_version: 1
id: FIL-11B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-11
function: log_filter
proposed_operation_keys:
- filter.log_filter_strict
- filter.log_filter_accelerated_apple_silicon
- filter.log_filter_accelerated_x86_64
numeric_reference: B+E
oracle_scope: mathematical_reference
---

# FIL-11B: log_filter

Discrete zero-DC LoG filter.

Inherits [FIL-11](FIL-11_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.log_filter_strict`, `filter.log_filter_accelerated_apple_silicon`, `filter.log_filter_accelerated_x86_64`.

`input` → `output`; the operation internally uses the kernel defined by FIL-11A.

### Parameters

All FIL-11A parameters are explicit. `y_axis` and `x_axis` are explicit. The supported boundaries are explicitly selected from `constant`, `clamp`, `reflect_half`, `reflect_whole`, and `wrap`; `truncate` is not supported. For a constant boundary, `cval` is explicit.

## Mathematics

Numerical classification: **B+E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S.

`RN_t(Σ_{d≠0} v_d*(E(I,q+d)-I_q))`. This is an exact center correction; do not store `-Σoff v` in Float64 and then use it. `scale_normalized` affects FIL-11A. This is not equivalent to a Gaussian blur followed by a discrete Laplacian.

## Behavior

Support extends by `radius` around each output sample after global boundary mapping. Zero taps do not participate in source arithmetic.

## Acceptance

For finite constant inputs, `clamp`, reflection, and `wrap` must produce strict +0; `truncate` is unsupported. Under a constant boundary, output is zero only when `cval` equals the input constant. Compare impulse responses with the FIL-11A center correction.
