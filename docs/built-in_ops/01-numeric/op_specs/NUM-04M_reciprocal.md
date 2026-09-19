---
spec_schema_version: 1
id: NUM-04M
parent_id: NUM-04
function: reciprocal
proposed_operation_keys:
  - numeric.reciprocal_strict
  - numeric.reciprocal_accelerated_apple_silicon
  - numeric.reciprocal_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-04M: reciprocal

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute 1/x on Float32/Float64, preserving dtype and shape. Inherit the
[NUM-04 common contract](NUM-04_unary_contract.md) for the parameter-free
input/values interface, generic output, regional and typed-validation support,
NaN handling, ownership, budgets and public acceptance. Integer input rejects
with TypeMismatch and requires explicit conversion.

For finite nonzero x, return the mathematical reciprocal directly correctly
rounded to output dtype, with ties-to-even and gradual underflow. Correct
overflow yields signed infinity; correct underflow yields a signed subnormal
or signed zero. +0/-0 yield +Inf/-Inf, and +Inf/-Inf yield +0/-0. NaNs preserve
payload/sign with quieting. These are successful numeric results, not divide-
by-zero/domain/overflow Status failures.

All three versions must match bits; approximate reciprocal instructions alone
do not satisfy this contract. A verified correctly rounded division or an
approximation with proved refinement/correction is allowed. Any unresolved fast
case uses strict evaluation; no four-ULP allowance exists here. Classify special
values before native arithmetic and restore the host floating environment.
Scratch and actual refinement work are host-accounted under the common contract.

## Independent acceptance

Check ±1, ±2, ±0, ±Inf and multiple signed NaN payloads. For other finite x,
use exact rational 1/x and independently determine destination rounding, including
midpoint cases, smallest subnormal/normal input, largest finite input and the
overflow boundary. Compare every platform bitwise. No float-to-float approximate
reciprocal is a strict oracle.

Conceptual public fixture: `[2,-4,+0,-0,+Inf,-Inf]` ->
`[0.5,-0.25,+Inf,-Inf,+0,-0]`. Run through a compiled public workflow with actual
bindings, then apply common nonzero/disjoint ROI, read-witness, cache/owner,
cancellation and resource tests. There is no implementation of these new keys
claimed here; executable targets, commands and platform evidence are delivery work.

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
