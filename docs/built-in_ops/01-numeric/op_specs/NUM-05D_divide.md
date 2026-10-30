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
implementation_status: implemented
---

# NUM-05D: divide

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md) and the [binary contract](NUM-05_binary_contract.md); explicit rules below take precedence.

Computes elementwise `a/b`. Inputs `a` and `b` have identical shapes and dtypes. Only Float32 and Float64 are supported; integer input ports fail TypeMismatch at compile/preflight, and integer conversion is explicit. Output `values` preserves the input dtype and shape and has no attached domain description. There are no static parameters and no truncating or flooring integer quotient mode. Keys: `numeric.divide_strict`, `numeric.divide_accelerated_apple_silicon`, `numeric.divide_accelerated_x86_64`.

## Mathematics

For finite floating operands, strict correctly rounds the exact mathematical result directly to the output dtype, with ties to even and gradual underflow. Accelerated floating results obey the shared final FP32 four-ULP contract. Finite overflow produces signed infinity as a successful numeric output. Input NaNs follow the shared payload/sign priority before non-NaN special cases are considered; both operands are still read.

| Non-NaN operands | Result |
| --- | --- |
| Zero divided by zero, or infinity divided by infinity | Canonical positive quiet NaN |
| Finite nonzero divided by zero | Infinity with XOR operand sign |
| Infinity divided by finite (including zero) | Infinity with XOR operand sign |
| Finite divided by infinity | Zero with XOR operand sign |

Every zero or infinite non-NaN result has the XOR of operand signs, including underflow and signed-zero operands.

## Acceptance

Fixture: `[1,2,3] / [2,2,2] -> [0.5,1,1.5]`, repeated for both dtypes. Cases: signed-zero operand combinations, extrema, subnormal/normal boundaries, all infinity combinations, and NaNs with distinct payloads in both input orders. The reference is exact rational rounding.
