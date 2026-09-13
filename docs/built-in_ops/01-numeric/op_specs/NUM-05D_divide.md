---
spec_schema_version: 1
id: NUM-05D
parent_id: NUM-05
function: divide
proposed_operation_keys:
  - numeric.divide_strict
  - numeric.divide_accelerated_apple_silicon
  - numeric.divide_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: target_contract_not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-05D: divide

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute elementwise `a/b`. Inputs `a` and `b` must have identical shapes
and dtypes. Support Float32 and Float64 only; output `values` preserves
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
| Zero divided by zero, or infinity divided by infinity | Canonical positive quiet NaN |
| Finite nonzero divided by zero | Infinity with XOR operand sign |
| Infinity divided by finite (including zero) | Infinity with XOR operand sign |
| Finite divided by infinity | Zero with XOR operand sign |

Every zero or infinite non-NaN result has the XOR of operand signs, including
underflow and signed-zero operands.

Integer inputs require explicit cast. This function has no truncating or
flooring integer quotient mode.

## Acceptance and implementation gap

Conceptual public fixture: `[1,2,3] / [2,2,2] -> [0.5,1,1.5]`, repeated for supported dtypes.
Include signed-zero operand combinations, extrema, subnormal/normal boundaries,
all infinity combinations and NaNs with distinct payloads in both input orders.
Use exact rational rounding as the floating oracle. Integer input ports fail
TypeMismatch at compile/preflight. Requested floating overflow returns signed
infinity successfully; unrequested coordinates are not evaluated. Apply all
shared public execution and resource cases.

The current legacy operation uses finite-only Float32/Float64 arithmetic and
Whole execution. It does not implement this versioned IEEE-like contract or
establish the required per-coordinate support. No runtime implementation or
public execution test is delivered by this specification-only change.
