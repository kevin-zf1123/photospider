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
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-05C: multiply

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute elementwise `a*b`. Inputs `a` and `b` must have identical shapes
and dtypes. Support UInt8, Int64, Float32 and Float64; output `values` preserves
the input dtype and shape and has empty facets. There are no static numeric
parameters. Inherit the [binary contract](NUM-05_binary_contract.md), including
exact per-input demand, validation, resources, lifetime, errors and acceptance.

## Numeric semantics

For finite floating operands, strict correctly rounds the exact mathematical
result directly to the output dtype, with ties to even and gradual underflow.
Accelerated floating results obey the shared final FP32 4 ULP contract. Finite overflow produces signed infinity as a
successful numeric output. Input NaNs follow the shared payload/sign priority
before non-NaN special cases are considered; both source operands are still read.

| Non-NaN operands | Result |
| --- | --- |
| Zero times infinity, in either order | Canonical positive quiet NaN |
| Infinity times nonzero non-NaN | Infinity with XOR operand sign |

Every zero or infinite non-NaN result has the XOR of operand signs, including
underflow and signed-zero operands.

For UInt8/Int64, compute the exact integer result and reject only when that
result is outside the destination range. Overflow fails the requested observation
without wrapping, saturation or promotion. No floating conversion is permitted.

## Acceptance and legacy comparison

Conceptual public fixture: `[1,2,3] * [4,5,6] -> [4,10,18]`, repeated for supported dtypes.
Include signed-zero operand combinations, extrema, subnormal/normal boundaries,
all infinity combinations and NaNs with distinct payloads in both input orders.
Use exact rational rounding as the floating oracle and arbitrary-precision
integers for integer cases; verify requested overflow and unrequested overflow
separately. Apply all shared public execution and resource cases.

The current legacy operation uses finite-only Float32/Float64 arithmetic and
Whole execution. It does not implement this versioned IEEE-like contract or
establish the required per-coordinate support. The maintained versioned implementation and public example are described below.

## Maintained implementation

The three keys are registered by `plugins/ops/01-numeric/numeric_binary.cpp`,
with independently named constructors in `photospider/numeric/binary.hpp`.
The shared adapter retains both inputs as exact pointwise Data and separately
retains typed validation, including when a numeric identity determines a result.
Floating elementary operations use controlled correctly rounded hardware
arithmetic after exact special-value classification, with exact fallback; integer
operations retain checked exact arithmetic. Accelerated ordinary positive-base
power and angle results use SLEEF binary64 enclosures within the shared admitted
ranges. atan2pi divides an angle enclosure by an enclosed pi. Only rejected
candidates dispatch the certified strict backend and report strict fallback. See [math implementation](../math-implementation.md) for
rounding, scratch, work accounting and unresolved-refinement limits.

The [public example and commands](../../../../examples/numeric_workflow/README.md)
include this operation, an editable add/multiply composition, independent
integer/Fraction/MPFR oracles and direct error/resource checks. The manual target
is excluded from default builds and has no CTest/integration registration.

The combined family passed 14,174 independent cases per profile on native
Clang strict/Apple and Ubuntu WSL Clang strict/AVX2, plus expanded manual and
local installed-consumer checks. [Measured validation scope](../math-implementation.md#num-05-validation-and-native-timing)
records oracle versions, native timings and limitations.
