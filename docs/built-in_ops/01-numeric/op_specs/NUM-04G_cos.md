---
spec_schema_version: 1
id: NUM-04G
parent_id: NUM-04
function: cos
proposed_operation_keys:
  - numeric.cos_strict
  - numeric.cos_accelerated_apple_silicon
  - numeric.cos_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04G: cos

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md), the [trigonometric contract](NUM-04_trigonometric_contract.md) and the [unary contract](NUM-04_unary_contract.md); explicit rules below take precedence.

Computes cos(x), interpreting x as an exact floating-point value in radians. Input/output dtype is Float32 or Float64 and shape is unchanged. The output is `values`, with no attached domain description; there are no static parameters. Keys: `numeric.cos_strict`, `numeric.cos_accelerated_apple_silicon`, `numeric.cos_accelerated_x86_64`.

## Mathematics

| Input | Output |
| --- | --- |
| +0 / -0 | Exactly 1 |
| +-Inf | Fixed positive quiet NaN |
| NaN | Quiet input NaN, preserving payload/sign |

Other finite inputs use the shared correctly rounded strict or <=4-ULP accelerated contract.
Cosine is numerically even; input NaN sign preservation follows the common payload rule rather than an arithmetic evenness claim. Output stays in [-1,1] as well as meeting its ULP bound.

## Acceptance

Fixture: `[+0,-0]` gives `[1,1]`.

Nontrivial finite fixture: input `[1]` is exactly one radian. Strict cos(1) has Float32 bits `0x3f0a5140` and Float64 bits `0x3fe14a280fb5068c`. Accelerated results obey the shared four-ULP bound; 1 radian carries no special-angle identity. The reference bits come from independent rational enclosures of the series, with direct destination rounding.

Also test small/large finite radian inputs, signed zero, NaN payloads, infinity, and neighbors of rounded pi multiples; strict compares bits, accelerated compares ULP distance and classification.
