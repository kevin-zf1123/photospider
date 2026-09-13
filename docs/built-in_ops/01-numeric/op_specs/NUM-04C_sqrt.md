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
implementation_status: not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-04C: sqrt

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute the nonnegative square root for each observed Float32/Float64 value,
preserving shape and dtype. Inherit the [NUM-04 common contract](NUM-04_unary_contract.md)
for the parameter-free input/values interface, regional/typed-validation demand,
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

All three implementations must return identical bits; there is no accelerated
ULP allowance for sqrt. Float32 output is defined by rounding the mathematical
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
and destination rounding boundaries. Compare all three keys bitwise to an
independent oracle. Include caller rounding-mode/underflow settings, disjoint
ROI without reading remote samples, source mutations, cache-off, and common
ownership/cancellation/budget tests.

Conceptual public fixture: input `[-1,-0,0,4,+Inf]` produces
`[canonical_NaN,-0,+0,2,+Inf]` with the same dtype and no facets. The new keys
are not implemented; delivery must supply the actual public executable and
run commands. NUM-01's expression sqrt has a different input/output/error contract.
