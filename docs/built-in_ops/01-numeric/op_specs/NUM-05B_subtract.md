---
spec_schema_version: 1
id: NUM-05B
parent_id: NUM-05
function: subtract
proposed_operation_keys:
  - numeric.subtract_strict
  - numeric.subtract_accelerated_apple_silicon
  - numeric.subtract_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: target_contract_not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-05B: subtract

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute elementwise `a-b`. Inputs `a` and `b` must have identical shapes
and dtypes. Support UInt8, Int64, Float32 and Float64; output `values` preserves
the input dtype and shape and has empty facets. There are no static numeric
parameters. Inherit the [binary contract](NUM-05_binary_contract.md), including
exact per-input demand, validation, resources, lifetime, errors and acceptance.

## Numeric semantics

For finite floating operands, correctly round the exact mathematical result
directly to the output dtype, with ties to even and gradual underflow. All three
profiles produce identical bits. Finite overflow produces signed infinity as a
successful numeric output. Input NaNs follow the shared payload/sign priority
before non-NaN special cases are considered; both source operands are still read.

| Non-NaN operands | Result |
| --- | --- |
| Same signed infinities | Canonical positive quiet NaN |
| Opposite signed infinities | Infinity with sign(a) |
| Finite minus infinity | Infinity with opposite sign(b) |

Zero signs follow addition of a and the sign-negated b: -0 minus +0
is -0; the other signed-zero pairs and exact nonzero cancellation produce +0.
This sign rule does not negate a propagated input NaN.

For UInt8/Int64, compute the exact integer result and reject only when that
result is outside the destination range. Overflow fails the requested observation
without wrapping, saturation or promotion. No floating conversion is permitted.

## Acceptance and implementation gap

Conceptual public fixture: `[4,5,6] - [1,2,3] -> [3,3,3]`, repeated for supported dtypes.
Include signed-zero operand combinations, extrema, subnormal/normal boundaries,
all infinity combinations and NaNs with distinct payloads in both input orders.
Use exact rational rounding as the floating oracle and arbitrary-precision
integers for integer cases; verify requested overflow and unrequested overflow
separately. Apply all shared public execution and resource cases.

The current legacy operation uses finite-only Float32/Float64 arithmetic and
Whole execution. It does not implement this versioned IEEE-like contract or
establish the required per-coordinate support. No runtime implementation or
public execution test is delivered by this specification-only change.
