---
spec_schema_version: 1
id: NUM-04L
parent_id: NUM-04
function: sign
proposed_operation_keys:
  - numeric.sign_strict
  - numeric.sign_accelerated_apple_silicon
  - numeric.sign_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04L: sign

Numeric profile: strict is the exact reference defined below. Discrete results are exact and bitwise identical across the three keys. Inherits the [NUM baseline](NUM_common_contract.md) and the [NUM-04 unary contract](NUM-04_unary_contract.md); explicit rules below take precedence.

Returns -1 for a negative nonzero number, +1 for a positive number, and zero for zero, preserving input shape and dtype. UInt8, Int64, Float32 and Float64 are supported. There are no static parameters or implicit output conversions. Keys: `numeric.sign_strict`, `numeric.sign_accelerated_apple_silicon`, `numeric.sign_accelerated_x86_64`.

## Mathematics

Floating +-0 retain their input sign, +-Inf return +-1, and NaNs are quieted while preserving payload and sign. UInt8 outputs are only 0 or 1. Int64 extrema, including INT64_MIN, return their sign without negation or absolute value, so they do not overflow. Signed subnormals map to signed one.

## Acceptance

- Int64 `[INT64_MIN,-1,0,1,INT64_MAX]` gives `[-1,-1,0,1,1]`; all UInt8 inputs map zero to zero and every nonzero value to one.
- Float `[-Inf,-3,-0,+0,3,+Inf]` gives `[-1,-1,-0,+0,1,1]` with preserved dtype.
- Positive and negative NaN payloads survive with quieting. Reapplying sign is bitwise idempotent.
