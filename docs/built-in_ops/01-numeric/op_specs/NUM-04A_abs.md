---
spec_schema_version: 1
id: NUM-04A
parent_id: NUM-04
function: abs
proposed_operation_keys:
  - numeric.abs_strict
  - numeric.abs_accelerated_apple_silicon
  - numeric.abs_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04A: abs

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md) and the [NUM-04 unary contract](NUM-04_unary_contract.md); explicit rules below take precedence.

Computes the per-element absolute value, preserving input shape and dtype. Ports, output schema and NaN handling follow the unary contract. There are no static operation parameters. Keys: `numeric.abs_strict`, `numeric.abs_accelerated_apple_silicon`, `numeric.abs_accelerated_x86_64`; SDK helpers are in `photospider/ops/numeric/unary.hpp`.

## Mathematics

| Input dtype | Exact output behavior |
| --- | --- |
| UInt8 | Identity bit copy |
| Int64 | x if x>=0, otherwise mathematical -x; the caller excludes INT64_MIN because its magnitude does not fit Int64 |
| Float32/Float64 | Absolute value, clearing the sign bit; an input signaling NaN is quieted with its payload preserved |

Floating -0 becomes +0, negative infinity becomes positive infinity, and NaNs have a cleared sign and set quiet bit. Finite float magnitudes do not change, including subnormals. There is no clipping or saturating integer mode. All three versions give identical result bits.

Representable integer magnitude is an unchecked caller precondition. UInt8 is valid across its full range.

## Acceptance

- UInt8: all 256 inputs return identical bits.
- Int64: `[-7,0,7,INT64_MAX]` gives `[7,0,7,INT64_MAX]`.
- Float32: raw `0x80000000` -> `0x00000000`, `0xff800000` -> `0x7f800000`, and signaling NaN `0xff812345` -> quiet `0x7fc12345`.
- Float64: `0xfff0000000000042` -> `0x7ff8000000000042`; normal/subnormal positive magnitude bits are unchanged.
- Idempotence: abs(abs(x)) has the same bits as abs(x), including quieted NaNs.

Fixture: Int64 `[-3,0,4]` gives `[3,0,4]`.
