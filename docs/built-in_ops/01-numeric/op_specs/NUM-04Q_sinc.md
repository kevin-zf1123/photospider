---
spec_schema_version: 1
id: NUM-04Q
parent_id: NUM-04
function: sinc
proposed_operation_keys:
  - numeric.sinc_strict
  - numeric.sinc_accelerated_apple_silicon
  - numeric.sinc_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04Q: sinc

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md), the [trigonometric contract](NUM-04_trigonometric_contract.md) and the [unary contract](NUM-04_unary_contract.md); explicit rules below take precedence.

For finite nonzero x, computes the mathematical `sin(x)/x`. The input float is its exact real value; pi denotes exact mathematical pi. Extends the function at both signed zeros by +1 and at both infinities by +0. Input NaNs preserve payload and sign and are quieted. All of these are successful numeric outputs. This is an independently named operation with no normalization or angle-unit parameter.

## Interface

One input `input` and output `values` have identical shape and dtype, Float32 or Float64. Integer inputs require explicit cast. The output carries no domain description. Both platform accelerated keys reject incompatible platforms.
Keys: `numeric.sinc_strict`, `numeric.sinc_accelerated_apple_silicon`, `numeric.sinc_accelerated_x86_64`.

## Mathematics

Strict correctly rounds the entire mathematical quotient directly to the output dtype; it is not a rounded sine followed by a rounded division. Accelerated finite nonzero results permit at most four FP32-scaled ULP from strict; NaN/Inf/zero classification and sign and specified landmarks match strict exactly. The bound applies to the whole function, including intermediate overflow behavior and accuracy around zero.

The function is even for real inputs. Nonzero exact results that underflow to zero retain their mathematical sign. NaN sign preservation follows the common bit policy independently of mathematical evenness. No finite nonzero binary floating input is an exact root of sin(x); rounded approximations to multiples of pi are not snapped to zero.

## Acceptance

Independent directed high-precision reference for the whole quotient. Cases: +-0 gives +1, +-Inf gives +0, NaN payloads, tiny subnormals, the transition away from the flat region around one, largest finite inputs, oscillatory sign changes and hard rounding cases. Exact special bits and accelerated final error are verified on the final quotient.

Fixture: `[-0,+0,-Inf,+Inf]` gives `[1,1,+0,+0]`; NaN fixtures are checked separately using exact input payload bits.
