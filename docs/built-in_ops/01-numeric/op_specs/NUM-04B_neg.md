---
spec_schema_version: 1
id: NUM-04B
parent_id: NUM-04
function: neg
proposed_operation_keys:
  - numeric.neg_strict
  - numeric.neg_accelerated_apple_silicon
  - numeric.neg_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04B: neg

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md) and the [NUM-04 unary contract](NUM-04_unary_contract.md); explicit rules below take precedence.

Negates each numeric value. Input and output dtype are the same, restricted to Int64, Float32 or Float64; UInt8 is rejected with TypeMismatch and requires an explicit cast. There are no static parameters. Keys: `numeric.neg_strict`, `numeric.neg_accelerated_apple_silicon`, `numeric.neg_accelerated_x86_64`.

## Mathematics

- Int64 computes mathematical -x. The caller excludes INT64_MIN because +2^63 is not representable.
- Floating values flip the sign bit, including zero, infinity and NaN. A signaling NaN is quieted with its payload preserved. Thus +0 becomes -0, -0 becomes +0, and the signs of infinities swap.
- Floating negation does not overflow, and no finite/nonfinite filtering or approximation applies. All three keys are bitwise equivalent.

NUM-01's expression unary minus is a different interface with its own finite-expression policy.

## Acceptance

- Int64 `[-7,0,7,INT64_MAX]` -> `[7,0,-7,-INT64_MAX]`. UInt8 inputs are rejected before data execution.
- Float32 `0x00000000` -> `0x80000000`, `0xff800000` -> `0x7f800000`, and signaling NaN `0x7f812345` -> `0xffc12345`.
- Float64 `0xfff0000000000042` -> `0x7ff8000000000042`; subnormal magnitude bits are unchanged.
- Double negation restores original bits except that an input signaling NaN remains quieted.

Fixture: Float64 `[1,-2,-0]` gives `[-1,2,+0]`.
