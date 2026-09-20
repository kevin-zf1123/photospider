---
spec_schema_version: 1
id: NUM-04L
parent_id: NUM-04
function: sign
proposed_operation_keys:
  - numeric.sign_strict
  - numeric.sign_accelerated_apple_silicon
  - numeric.sign_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-04L: sign

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Return -1 for a negative nonzero number, +1 for a positive number, and zero for
zero, preserving input shape and dtype. UInt8/Int64/Float32/Float64 are supported.
Inherit [NUM-04 common execution and validation](NUM-04_unary_contract.md);
there are no static operation parameters or implicit output conversions.

Floating ±0 retain their input sign, ±Inf return ±1, and NaNs are quieted while
preserving payload/sign. UInt8 outputs are only 0 or 1. Int64 extrema, including
INT64_MIN, return their sign without negation or absolute value, so they do not
overflow. All three versions must return the same logical bits.

Classify integers by comparison without a floating intermediate; classify float
NaNs/zeros by bits before selecting signed unit output. Avoid formulas x/abs(x)
that fail on zero, infinity or INT64_MIN. Work is O(M), element scratch O(1), and
payload M*b plus common mapping and typed-validation support. Every generic
numeric input has a defined successful numeric result.

## Independent acceptance

- Int64 `[INT64_MIN,-1,0,1,INT64_MAX]` -> `[-1,-1,0,1,1]`; all UInt8 inputs
  map zero to zero and every nonzero value to one.
- Float `[-Inf,-3,-0,+0,3,+Inf]` -> `[-1,-1,-0,+0,1,1]` with preserved dtype.
- Positive/negative NaN payloads survive with quieting; signed subnormals map
  to signed one without flushing to zero. Reapplying sign is bitwise idempotent.
- Compare platform keys to an independent bit/comparison oracle and run the
  inherited public ROI/read-witness/cache/lifetime/resource cases.

The conceptual public fixture binds the float vector above, compiles a selected
sign key and checks output bytes and dtype. The maintained target implementation is documented below;
the maintained public executable command and current validation boundary are documented below.

## Maintained implementation and validation

This operation is registered in `plugins/ops/01-numeric/numeric_unary.cpp` and
exposed through `photospider/numeric/unary.hpp`. It uses synchronous Whole execution, full-input typed validation and
atomic failure for the complete invocation. Its numerical path follows
[the shared implementation notes](../math-implementation.md).

The [public workflow and commands](../../../../examples/numeric_workflow/README.md)
cover this operation. The combined NUM-04 family suite passed 7,524 independent
integer/Fraction/MPFR cases per profile: Clang 21 strict/Apple locally (MPFR 4.2.0-p12; revalidated 2026-09-21)
and Clang 18 strict/AVX2 in Ubuntu WSL (MPFR 4.2.1). Expanded manual checks and
local installed consumers passed. [Validation and native timing](../math-implementation.md#num-04-validation-and-native-timing)
record the scope and limitations. Manual targets have no CTest/integration
registration; MPFR is used only by the independent Python oracle.
