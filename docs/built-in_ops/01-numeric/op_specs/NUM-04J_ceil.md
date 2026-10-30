---
spec_schema_version: 1
id: NUM-04J
parent_id: NUM-04
function: ceil
proposed_operation_keys:
  - numeric.ceil_strict
  - numeric.ceil_accelerated_apple_silicon
  - numeric.ceil_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04J: ceil

Numeric profile: strict is the exact reference defined below. Discrete results are exact and bitwise identical across the three keys. Inherits the [NUM baseline](NUM_common_contract.md) and the [NUM-04 unary contract](NUM-04_unary_contract.md); explicit rules below take precedence.

For a floating x, returns the least integer greater than or equal to x, stored in the original floating dtype. UInt8 and Int64 are exact identity. Shape and dtype are preserved; there are no static parameters or implicit conversion to an integer output array. Keys: `numeric.ceil_strict`, `numeric.ceil_accelerated_apple_silicon`, `numeric.ceil_accelerated_x86_64`.

## Mathematics

Negative finite inputs above -1 yield -0; positive fractional values yield the next higher integer. Both input signed zeros keep their sign, and any finite zero result uses the input sign. Infinities return unchanged; input NaNs are quieted with payload and sign preserved. Integer extrema are copied exactly and never pass through Float64. Finite floating numbers whose representation is already integral return unchanged. The result does not depend on the caller's rounding mode. No arithmetic overflow or ULP tolerance applies.

## Acceptance

Fixture: `[-1.5,-0.25,-0,+0,1.5]` gives `[-1,-0,-0,+0,2]`. The output dtype is unchanged. Cases: all UInt8 values, Int64 values above 2^53 and extrema, floating neighbors of integers, subnormals, signed zeros, infinities and NaN payloads, compared against an independent rational integral reference with explicit bit handling.
