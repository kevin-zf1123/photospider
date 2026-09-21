---
spec_schema_version: 1
id: CRV-08A
parent_id: CRV-08
function: linear_shaper
proposed_template_names:
  - curve.linear_shaper
category: 01-numeric
kind: composite_workflow
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-08A: linear_shaper

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and formula

Ordered dynamic ports are input, lower[1], upper[1], all same Float32/Float64
dtype. Output values preserves input dtype/shape with empty facets. Input rank
is 1..8, positive extents and logical count <=2^40. Bounds are shared finite
scalars satisfying lower<upper. No implicit clipping occurs.
The mathematical formula is (input-lower)/(upper-lower), with whole-expression final rounding.
This named authoring template expands remap_range and explicit broadcast/constant nodes. Static profile is strict/apple_silicon/x86_64, default strict; it does not register a new primitive.


Inherit the [complete shaper contract](CRV-08_shaper.md) for exact endpoints,
zero signs, NaN quieting/payloads, infinity/domain extensions and overflow as
successful IEEE results. Bound validation always precedes source special values.
Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound.
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

## Public workflow and acceptance

lower=-2, upper=2, input=[-2,0,2,4] -> [0,0.5,1,1.5].
Bind these ports through the public Compiler/ExecutionContext interface
after template expansion and check named values.
The maintained public workflow executes this fixture through Compiler/ExecutionContext.

Apply the shared independent oracle, endpoint/extreme/subnormal/NaN fixtures,
partial-request and dirty witnesses, strides, resource limits, cancellation,
cache-off and lifetime checks. Use an exact rational whole-formula oracle; inverse acceptance includes lower>=upper rejection.
The maintained target command and current validation boundary are linked below; the
specification remains Proposed.

## Maintained implementation and validation

The public entry point is `linear_shaper` in `photospider/numeric/shapers.hpp`. This linear helper expands to existing remap/constant nodes and is not a linear primitive.
See [the CRV-08 family contract](CRV-08_shaper.md) and [the shaper workflow README](../../../../examples/numeric_workflow/README.md)
for the shared command, fixture and validation evidence. Native Clang 21
strict/Apple and Ubuntu WSL Clang 18 strict/AVX2 passed 4,196 independent
Fraction/directed-MPFR cases per profile. All five manual groups passed
all four profiles.
