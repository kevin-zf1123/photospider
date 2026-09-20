---
spec_schema_version: 1
id: NUM-07G
parent_id: NUM-07
function: is_close
proposed_operation_keys:
  - numeric.is_close_strict
  - numeric.is_close_accelerated_apple_silicon
  - numeric.is_close_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-impl
repository_commit: current working tree
---

# NUM-07G: is_close

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compare matching Float32/Float64 inputs `a` and `b` using the symmetric formula

    abs(a-b) <= atol + rtol * max(abs(a),abs(b))

The two input arrays have identical dtype and shape. Output `values` is UInt8
with unchanged shape and empty facets: 1 means close, 0 means not close. Static
parameters `atol` and `rtol` are required Float64 values, finite and nonnegative,
with no implicit default. Negative zero is numerically zero. Reject missing,
nonfinite or negative tolerance at compile/preflight. No other static numeric
parameters, implicit cast or broadcast is provided.

Inherit the [comparison execution contract](NUM-07_comparison_contract.md) for
both-input support, typed validation, errors, resources, mapping and lifetime.

## Exact predicate and special cases

Finite inputs and tolerances denote their exact binary rational values. Compare
the entire formula exactly, including the equality boundary, with no intermediate
floating rounding, overflow or underflow. All three profiles yield identical
UInt8 bits. Decode significands/exponents and use exact scaled-integer comparison
or a certified equivalent; charge actual arithmetic work and scratch to budgets.

Before the finite formula: any NaN yields 0; same-sign infinities yield 1;
otherwise an infinity paired with any different value yields 0. Both signed
zeros yield 1. NaN payloads do not affect this boolean output. All generic
numeric inputs produce successful results unless execution/validation/resources
fail; no floating domain Status is introduced. Typed input validation still
applies, and both operands are read even when classification decides the result.

## Acceptance

Conceptual fixture: a=[1,2,3], b=[1,2.25,4], atol=0.25, rtol=0 yields [1,1,0].
Check symmetry, exact tolerance boundaries and adjacent floats, zero tolerances,
signed zeros, infinities, all NaN positions and unequal input-dtype rejection.
For a=MAX_FLOAT64, b=-MAX_FLOAT64, atol=0 and rtol=1.5, the result is 0;
naive floating arithmetic can overflow both sides to infinity and incorrectly
return 1. Verify this by independent exact rational arithmetic, alongside tiny
subnormal differences and products. No epsilon is added to the comparison.

Exercise a public WorkflowDocument with static tolerances, both inputs and
requested output coordinates when implemented. Apply the inherited strided,
disjoint, typed-validation, lifetime, cache and budget/cancellation cases;
resource exhaustion must fail rather than guess a near-threshold predicate.

The default registry implements all three profile keys using a bounded exact
4352-bit predicate workspace. On 2026-09-14, the 3760-case independent Fraction
oracle, including maximum relative tolerances and underflow/equality boundaries,
passed with AppleClang 21 strict/Apple and Ubuntu WSL Clang 18 strict/x86.
The installed public workflow passed, including `MAX/-MAX` with rtol=1.5,
Empty/tolerance validation, work/state exhaustion and cancellation cleanup.
The specification remains Proposed; these are manual checks outside integration
tests, with no performance claim.
