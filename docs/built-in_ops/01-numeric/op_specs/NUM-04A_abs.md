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
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-04A: abs

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute per-element absolute value, preserving input shape and dtype. Inherit
the [NUM-04 common contract](NUM-04_unary_contract.md) for ports, generic output,
regional data/validation demand, NaN handling, ownership, budgets and public
acceptance. There are no static operation parameters.

## Selected mathematical and type contract

| Input dtype | Exact output behavior |
| --- | --- |
| UInt8 | Identity bit copy |
| Int64 | x if x>=0, otherwise mathematical -x; INT64_MIN fails because its positive magnitude does not fit Int64 |
| Float32/Float64 | Absolute value, clearing the sign bit; quiet an input signaling NaN while preserving its payload |

Floating -0 becomes +0, negative infinity becomes positive infinity, and NaNs
have a cleared sign and set quiet bit. Finite float magnitudes do not change,
including subnormals. No clipping or saturating integer mode is included.
All three versions have identical logical result bits and semantic failures.

An Int64-minimum failure is OperationFailed/ArithmeticOverflow for its actually
requested atom. Other requested observations can succeed under the host's atom
API; ordinary execute remains fail-fast. Reading an unrequested INT64_MIN is not
authorized. UInt8 cannot produce an arithmetic error, and floating NaN/Inf is a
successful numeric result subject to any preexisting typed-input obligations.

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
- Int64: `[-7,0,7,INT64_MAX]` gives `[7,0,7,INT64_MAX]`; INT64_MIN fails only
  where requested, without saturation or wraparound.
- Float32: raw `0x80000000` -> `0x00000000`, `0xff800000` -> `0x7f800000`,
  and signaling NaN `0xff812345` -> quiet `0x7fc12345`.
- Float64: `0xfff0000000000042` -> `0x7ff8000000000042`; normal/subnormal
  positive magnitude bits are unchanged by abs.
- Idempotence: abs(abs(x)) has the same output bits as abs(x), including quieted
  NaNs. All three implementations match the independent integer/bit-mask oracle.
- Run the inherited public workflow/lifetime/resource tests with a source
  containing a remote INT64_MIN and NaN payloads; inspect actual ROI reads.

Conceptual public fixture: bind Int64 `[-3,0,4]` to input, compile the selected
abs key, request values, and check `[3,0,4]`; repeat with nonzero regions and
changed bindings. The delivery must provide the actual executable/command.

## Current implementation difference

The current `numeric.abs` accepts only finite Float32/64, preserves shape/dtype,
and uses Elementwise registration. Its shared reader/writer rejects NaN/Inf;
the new target adds UInt8/Int64 and the selected nonfinite/payload semantics.
No removal/migration of the legacy key is performed by this specification.

- [Current registration/callback](../../../../plugins/ops/01-numeric/numeric_abs.cpp).
- [Current finite reader/writer](../../../../plugins/ops/00-foundation/basic_common.hpp).
- [Existing tests](../../../../tests/integration/test_basic_operations.cpp).

## Maintained implementation and validation

This operation is registered in `plugins/ops/01-numeric/numeric_unary.cpp` and
exposed through `photospider/numeric/unary.hpp`. It uses exact pointwise Data,
separate typed validation and Atom-scoped failures. Its numerical path follows
[the shared implementation notes](../math-implementation.md).

The [public workflow and commands](../../../../examples/numeric_workflow/README.md)
cover this operation. The combined NUM-04 family suite passed 7,524 independent
integer/Fraction/MPFR cases per profile: Clang 21 strict/Apple locally (MPFR 4.2.2)
and Clang 18 strict/AVX2 in Ubuntu WSL (MPFR 4.2.1). Expanded manual checks and
local installed consumers passed. [Validation and native timing](../math-implementation.md#num-04-validation-and-native-timing)
record the scope and limitations. Manual targets have no CTest/integration
registration; MPFR is used only by the independent Python oracle.
