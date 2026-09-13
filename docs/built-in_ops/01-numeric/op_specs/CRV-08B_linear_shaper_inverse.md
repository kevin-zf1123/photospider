---
spec_schema_version: 1
id: CRV-08B
parent_id: CRV-08
function: linear_shaper_inverse
proposed_template_names:
  - curve.linear_shaper_inverse
category: 01-numeric
kind: composite_workflow
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-08B: linear_shaper_inverse

## Interface and formula

Ordered dynamic ports are input, lower[1], upper[1], all same Float32/Float64
dtype. Output values preserves input dtype/shape with empty facets. Input rank
is 1..8, positive extents and logical count <=2^40. Bounds are shared finite
scalars satisfying lower<upper. No implicit clipping occurs.
The mathematical formula is lower+input*(upper-lower), with whole-expression final rounding.
This named authoring template expands remap_range and explicit broadcast/constant nodes. Static profile is strict/apple_silicon/x86_64, default strict; it does not register a new primitive.
The mandatory inverse lower<upper guard is the scalar remap_range composition specified in CRV-08; do not inherit permissive target-bound order unchecked.

Inherit the [complete shaper contract](CRV-08_shaper.md) for exact endpoints,
zero signs, NaN quieting/payloads, infinity/domain extensions and overflow as
successful IEEE results. Bound validation always precedes source special values.
All profiles are bitwise identical.
These operations do not modify a color description or implement tone mapping.

## Execution and failures

For nonempty Q, read input[Q] and both bounds even at exact endpoints or NaN.
Empty Q reads no payload. Inherit exact demand/dirty mapping, typed/upstream
closures, immutable packed Region origins, arbitrary source strides, owner
lifetime, cache-off and capacity/work/stage/cancellation obligations from CRV-08.
Account all internal guard/broadcast/remap state, not only the exported output.
Invalid bounds use InvalidArgument/InvalidDomain; shape/dtype mismatch uses
TypeMismatch. Numeric special results succeed. Resource, backend, stale,
upstream and cancellation failures preserve the shared categories and atom scope.

## Conceptual workflow and acceptance

lower=-2, upper=2, input=[0,0.5,1,1.5] -> [-2,0,2,4].
Bind these ports through the future public Compiler/ExecutionContext interface
after template expansion and check named values.
This is a conceptual fixture, not an already executed runtime workflow.

Apply the shared independent oracle, endpoint/extreme/subnormal/NaN fixtures,
partial-request and dirty witnesses, strides, resource limits, cancellation,
cache-off and lifetime checks. Use an exact rational whole-formula oracle; inverse acceptance includes lower>=upper rejection.
Actual public invocation commands and results are required for implementation
delivery. This Proposed document establishes no current runtime registration.
