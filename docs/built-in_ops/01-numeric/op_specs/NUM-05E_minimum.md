---
spec_schema_version: 1
id: NUM-05E
parent_id: NUM-05
function: minimum
proposed_operation_keys:
  - numeric.minimum_strict
  - numeric.minimum_accelerated_apple_silicon
  - numeric.minimum_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-05E: minimum

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md) and the [binary contract](NUM-05_binary_contract.md); explicit rules below take precedence.

Returns the lesser value of matching elements in inputs `a` and `b`. UInt8, Int64, Float32 and Float64 are supported; shapes and dtypes match exactly. Output `values` preserves shape and dtype with no attached domain description. There are no static parameters. Keys: `numeric.minimum_strict`, `numeric.minimum_accelerated_apple_silicon`, `numeric.minimum_accelerated_x86_64`.

## Mathematics

All three profiles are bitwise identical. Integer comparison selects the value exactly, including Int64 extrema, without conversion to floating point. Floating infinities participate in the ordinary numerical order.

If either operand is NaN, that input's payload and sign propagate, quieted. If both are NaN, the first input `a` propagates under the shared priority rule. This is not a NaN-ignoring operation. Both operands remain dependencies even if a NaN determines the output.

For mixed signed zeros, return -0. For same-sign zeros, retain that sign. For equal nonzero operands, return their common value. These selection rules introduce no rounding tolerance or overflow failure.

## Acceptance

a=[-2,0,5], b=[1,0,3] gives [-2,0,3], in ordinary and disjoint requests. Cases: all four signed-zero combinations, infinities, each single-NaN placement, two NaNs with different payload/sign bits, signaling NaNs, and Int64 values beyond Float64's exact range, against an independent integer/bit-classification reference.
