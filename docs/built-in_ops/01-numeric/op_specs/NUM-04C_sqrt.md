---
spec_schema_version: 1
id: NUM-04C
parent_id: NUM-04
function: sqrt
proposed_operation_keys:
  - numeric.sqrt_strict
  - numeric.sqrt_accelerated_apple_silicon
  - numeric.sqrt_accelerated_x86_64
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

# NUM-04C: sqrt

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute the nonnegative square root for each observed Float32/Float64 value,
preserving shape and dtype. Inherit the [NUM-04 common contract](NUM-04_unary_contract.md)
for the parameter-free input/values interface, Whole/typed-validation demand,
NaN propagation, generic output, lifetime and resource/public acceptance rules.
Integer input is TypeMismatch; conversion must be explicit.

## Exact numeric behavior

| Input | Output |
| --- | --- |
| Positive finite x | Correctly rounded mathematical sqrt(x), directly in input/output dtype |
| +0 / -0 | Same signed zero |
| +infinity | +infinity |
| Negative finite x or -infinity | Canonical positive quiet NaN from the common contract; successful observation |
| NaN | Quiet input NaN, preserving payload and sign |

Strict requires correctly rounded bits. Accelerated sqrt follows the shared
FP32-scaled final-result bound. Float32 output is defined by rounding the mathematical
root directly to Float32, not by an assumed-safe intermediate conversion.
All generic numeric inputs have a defined numeric result. Domain cases do not
raise OperationFailed; shape/type, typed-input, resource and upstream failures
retain the common Status behavior.

## Algorithm and acceptance

A hardware/software sqrt is acceptable when it establishes the selected
correct-rounding and floating-environment behavior. An independent exact oracle
can locate the destination candidate by comparing x against squares of adjacent
rounding midpoints with exact binary-rational arithmetic, including ties-to-even.
Do not accept a small residual as proof of rounding. Classify special inputs by
bits first to preserve NaNs and avoid host traps. Any unresolved accelerated
rounding uses an exact path; no weaker approximate result is published.

Finite input/output formats bound the reference arithmetic sizes. Cost is O(M)
primitive roots with implementation-specific constant/exact-rounding work and
O(1) element scratch, plus inherited input/output and validation resources.
Report and charge actual exact fallback work; poll long refinement if used.

Required checks: sqrt(0)=+0, sqrt(-0)=-0, sqrt(1)=1, sqrt(4)=2, correctly rounded
sqrt(2), negative inputs and -Inf canonical NaN, +Inf unchanged, payload-preserving
quieting of signed NaNs, smallest positive subnormal and largest finite input,
and destination rounding boundaries. Compare strict bits and accelerated final FP32-scaled accuracy to an
independent oracle. Include caller rounding-mode/underflow settings, disjoint
ROI without reading remote samples, source mutations, cache-off, and common
ownership/cancellation/budget tests.

Conceptual public fixture: input `[-1,-0,0,4,+Inf]` produces
`[canonical_NaN,-0,+0,2,+Inf]` with the same dtype and no facets. The maintained public executable and run command are documented below. NUM-01's
expression sqrt has a different input/output/error contract.

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
