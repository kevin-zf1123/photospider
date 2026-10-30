---
spec_schema_version: 1
id: NUM-04F
parent_id: NUM-04
function: sin
proposed_operation_keys:
  - numeric.sin_strict
  - numeric.sin_accelerated_apple_silicon
  - numeric.sin_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04F: sin

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md), the [trigonometric contract](NUM-04_trigonometric_contract.md) and the [unary contract](NUM-04_unary_contract.md); explicit rules below take precedence.

Computes sin(x), interpreting x as an exact floating-point value in radians. Input/output dtype is Float32 or Float64 and shape is unchanged. The output is `values`, with no attached domain description; there are no static parameters. Keys: `numeric.sin_strict`, `numeric.sin_accelerated_apple_silicon`, `numeric.sin_accelerated_x86_64`.

## Mathematics

| Input | Output |
| --- | --- |
| +0 / -0 | Same signed zero |
| +-Inf | Fixed positive quiet NaN |
| NaN | Quiet input NaN, preserving payload/sign |

Other finite inputs use the shared correctly rounded strict or <=4-ULP accelerated contract.
The sine of the Float64 approximation to pi is not forced to zero. The mathematical sine lies in [-1,1]; accelerated output also stays in this interval, in addition to its ULP bound.

## Acceptance

Fixture: `[+0,-0]` gives `[+0,-0]`.

Nontrivial finite fixture: input `[1]` is exactly one radian. Strict sin(1) has Float32 bits `0x3f576aa4` and Float64 bits `0x3feaed548f090cee`. Accelerated results obey the shared four-ULP bound; 1 radian carries no special-angle identity. The reference bits come from independent rational enclosures of the series, with direct destination rounding.

Also test small/large finite radian inputs, signed zero, NaN payloads, infinity, and neighbors of rounded pi multiples; strict compares bits, accelerated compares ULP distance and classification.
