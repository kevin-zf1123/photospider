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

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute per-element absolute value, preserving input shape and dtype. Inherit
the [NUM-04 common contract](NUM-04_unary_contract.md) for ports, generic output,
fixed tile reads and structural preparation, NaN handling, ownership, budgets and public
acceptance. There are no static operation parameters.

## Selected mathematical and type contract

| Input dtype | Exact output behavior |
| --- | --- |
| UInt8 | Identity bit copy |
| Int64 | x if x>=0, otherwise mathematical -x; the caller excludes INT64_MIN because its magnitude does not fit Int64 |
| Float32/Float64 | Absolute value, clearing the sign bit; quiet an input signaling NaN while preserving its payload |

Floating -0 becomes +0, negative infinity becomes positive infinity, and NaNs
have a cleared sign and set quiet bit. Finite float magnitudes do not change,
including subnormals. No clipping or saturating integer mode is included.
All three versions have identical logical result bits and semantic failures.

Representable integer magnitude is an unchecked caller precondition. UInt8 is valid across its full range; floating NaN and infinity follow the specified successful numeric rules.

## Reference algorithm and resource difference

For UInt8 copy bytes. For Int64, inspect for INT64_MIN before forming a signed
negation, then copy the exact result. Do not overflow and test afterward. For
Float32/64 load the unsigned bit representation, identify NaN, set its quiet
bit if necessary and clear the sign bit. Never convert Float32 NaNs through
Float64 or use a platform intrinsic that changes the selected payload rules.

Work is O(M) with O(1) element scratch plus inherited region/validation state.
Output allocation is M*b. Vectorized integer and bit-mask transforms must retain
exact NaN quieting and integer-minimum checks. No iterative math or approximate
fallback is required.

## Acceptance

- UInt8: all 256 inputs return identical bits.
- Int64: `[-7,0,7,INT64_MAX]` gives `[7,0,7,INT64_MAX]`; INT64_MIN violates the representable-magnitude precondition.
- Float32: raw `0x80000000` -> `0x00000000`, `0xff800000` -> `0x7f800000`,
  and signaling NaN `0xff812345` -> quiet `0x7fc12345`.
- Float64: `0xfff0000000000042` -> `0x7ff8000000000042`; normal/subnormal
  positive magnitude bits are unchanged by abs.
- Idempotence: abs(abs(x)) has the same output bits as abs(x), including quieted
  NaNs. All three implementations match the independent integer/bit-mask oracle.
- Run the inherited public workflow/lifetime/resource tests with a source
  containing representable integers and NaN payloads; inspect actual fixed tile reads.

Conceptual public fixture: bind Int64 `[-3,0,4]` to input, compile the selected
abs key, request values, and check `[3,0,4]`; repeat with nonzero regions and
changed bindings. The delivery must provide the actual executable/command.

## Current implementation difference

The unsuffixed `numeric.abs` uses fixed CPU tile tasks and an unchecked finite Float32/Float64 domain. The suffixed keys retain the IEEE and integer semantics above.

## Maintained implementation and validation

This operation is registered in `plugins/ops/01-numeric/numeric_unary.cpp` and
exposed through `photospider/ops/numeric/unary.hpp`. It uses one fixed CPU invocation per selected output tile, validated input views and preallocated output/scratch. Its numerical path follows
[the shared implementation notes](../math-implementation.md).
