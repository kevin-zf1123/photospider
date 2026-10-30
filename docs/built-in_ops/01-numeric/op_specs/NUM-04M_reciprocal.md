---
spec_schema_version: 1
id: NUM-04M
parent_id: NUM-04
function: reciprocal
proposed_operation_keys:
  - numeric.reciprocal_strict
  - numeric.reciprocal_accelerated_apple_silicon
  - numeric.reciprocal_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04M: reciprocal

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md), the [trigonometric contract](NUM-04_trigonometric_contract.md) and the [unary contract](NUM-04_unary_contract.md); explicit rules below take precedence.

Computes 1/x on Float32/Float64, preserving dtype and shape. Integer input is rejected with TypeMismatch and requires explicit conversion. There are no static parameters. Keys: `numeric.reciprocal_strict`, `numeric.reciprocal_accelerated_apple_silicon`, `numeric.reciprocal_accelerated_x86_64`.

## Mathematics

For finite nonzero x, the result is the mathematical reciprocal directly correctly rounded to the output dtype, with ties-to-even and gradual underflow. Correct overflow yields signed infinity; correct underflow yields a signed subnormal or signed zero. +0/-0 yield +Inf/-Inf, and +Inf/-Inf yield +0/-0. NaNs preserve payload and sign with quieting. These are successful numeric results, not failures.

Strict matches correctly rounded bits; accelerated follows the shared final FP32-scaled bound.

## Acceptance

+-1, +-2, +-0, +-Inf and several signed NaN payloads. For other finite x, exact rational 1/x with independently determined destination rounding, including midpoint cases, the smallest subnormal and normal input, the largest finite input and the overflow boundary.

Fixture: `[2,-4,+0,-0,+Inf,-Inf]` gives `[0.5,-0.25,+Inf,-Inf,+0,-0]`.
