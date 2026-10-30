---
spec_schema_version: 1
id: NUM-05C
parent_id: NUM-05
function: multiply
proposed_operation_keys:
  - numeric.multiply_strict
  - numeric.multiply_accelerated_apple_silicon
  - numeric.multiply_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-05C: multiply

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md) and the [binary contract](NUM-05_binary_contract.md); explicit rules below take precedence.

Computes elementwise `a*b`. Inputs `a` and `b` have identical shapes and dtypes. UInt8, Int64, Float32 and Float64 are supported; output `values` preserves the input dtype and shape and has no attached domain description. There are no static parameters. Keys: `numeric.multiply_strict`, `numeric.multiply_accelerated_apple_silicon`, `numeric.multiply_accelerated_x86_64`.

## Mathematics

For finite floating operands, strict correctly rounds the exact mathematical result directly to the output dtype, with ties to even and gradual underflow. Accelerated floating results obey the shared final FP32 four-ULP contract. Finite overflow produces signed infinity as a successful numeric output. Input NaNs follow the shared payload/sign priority before non-NaN special cases are considered; both operands are still read.

| Non-NaN operands | Result |
| --- | --- |
| Zero times infinity, in either order | Canonical positive quiet NaN |
| Infinity times nonzero non-NaN | Infinity with XOR operand sign |

Every zero or infinite non-NaN result has the XOR of operand signs, including underflow and signed-zero operands.

For UInt8/Int64, the caller supplies operands whose exact result fits the destination dtype (unchecked precondition). Valid results are computed exactly without floating conversion, wrapping, saturation or promotion.

## Acceptance

Fixture: `[1,2,3] * [4,5,6] -> [4,10,18]`, repeated for supported dtypes. Cases: signed-zero operand combinations, extrema, subnormal/normal boundaries, all infinity combinations, and NaNs with distinct payloads in both input orders. The floating reference is exact rational rounding; integer cases use arbitrary-precision integers.
