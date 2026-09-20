---
spec_schema_version: 1
id: NUM-04F
parent_id: NUM-04
function: sin
proposed_operation_keys:
  - numeric.sin_strict
  - numeric.sin_accelerated_apple_silicon
  - numeric.sin_accelerated_x86_64
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

# NUM-04F: sin

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute sin(x), interpreting x as an exact floating-point value in radians. Inherit all interface, rounding, error,
resource, demand, backend and acceptance requirements from the
[trigonometric contract](NUM-04_trigonometric_contract.md) and
[unary contract](NUM-04_unary_contract.md).

Input/output dtype is Float32 or Float64 and shape is unchanged. The named
output is values, with empty facets; there are no static numeric parameters.

## Function-specific values

| Input | Output |
| --- | --- |
| +0 / -0 | Same signed zero |
| ±Inf | Fixed positive quiet NaN |
| NaN | Quiet input NaN, preserving payload/sign |

Other finite inputs use the shared correctly rounded strict or <=4-ULP
accelerated contract, with the shared special-value requirements.
Radian finite inputs represent their actual rounded angle, so sin of the
Float64 approximation to pi is not forced to zero. The mathematical sine lies
in [-1,1]; accelerated output must also stay in this interval, in addition to
its ULP bound, without using a final clamp to excuse an unverified algorithm.

## Acceptance and current status

The conceptual public fixture is `[+0,-0]` -> `[+0,-0]`.
Build a WorkflowDocument with that floating array bound to input,
compile one selected key and inspect output bits
through ExecutionContext. Repeat in both dtypes and platform profiles, including
disjoint requests and exact source-read witnesses.

For a nontrivial finite fixture, input=[1] represents exactly one radian.
Strict sin(1) has Float32 bits `0x3f576aa4` and Float64 bits
`0x3feaed548f090cee`. Accelerated results obey the shared four-ULP bound;
this fixture does not turn 1 radian into a special-angle identity requirement.
Independent alternating-series rational enclosures for sin(1)/cos(1), with
positive-denominator interval division for tan(1), and direct destination
rounding establish these reference bits without using the production libm.

Also test ordinary small/large finite radian inputs, signed zero, NaN payloads,
infinity, neighbors of rounded pi multiples, independent correct
rounding and accelerated ULP/classification. Apply all shared resource, cache,
lifetime and cancellation cases. A real public executable, run commands and
the maintained public run command is documented below; local timing is recorded in the implementation notes.

## Maintained implementation and validation

This operation is registered in `plugins/ops/01-numeric/numeric_unary.cpp` and
exposed through `photospider/numeric/unary.hpp`. It uses exact pointwise Data,
separate typed validation and Atom-scoped failures. Its numerical path follows
[the shared implementation notes](../math-implementation.md).

The [public workflow and commands](../../../../examples/numeric_workflow/README.md)
cover this operation. The combined NUM-04 family suite passed 7,524 independent
integer/Fraction/MPFR cases per profile: Clang 21 strict/Apple locally (MPFR 4.2.0-p12; revalidated 2026-09-21)
and Clang 18 strict/AVX2 in Ubuntu WSL (MPFR 4.2.1). Expanded manual checks and
local installed consumers passed. [Validation and native timing](../math-implementation.md#num-04-validation-and-native-timing)
record the scope and limitations. Manual targets have no CTest/integration
registration; MPFR is used only by the independent Python oracle.
