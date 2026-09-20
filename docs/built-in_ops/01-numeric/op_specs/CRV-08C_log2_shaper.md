---
spec_schema_version: 1
id: CRV-08C
parent_id: CRV-08
function: log2_shaper
proposed_operation_keys:
  - curve.log2_shaper_strict
  - curve.log2_shaper_accelerated_apple_silicon
  - curve.log2_shaper_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-08C: log2_shaper

## Interface and formula

Ordered dynamic ports are input, lower[1], upper[1], all same Float32/Float64
dtype. Output values preserves input dtype/shape with empty facets. Input rank
is 1..8, positive extents and logical count <=2^40. Bounds are shared finite
scalars satisfying 0<lower<upper. No implicit clipping occurs.
The mathematical formula is (log2(input)-log2(lower))/(log2(upper)-log2(lower)), with whole-expression final rounding.
The independent operation key selects the CPU profile; there is no additional static mode parameter.


Inherit the [complete shaper contract](CRV-08_shaper.md) for exact endpoints,
zero signs, NaN quieting/payloads, infinity/domain extensions and overflow as
successful IEEE results. Bound validation always precedes source special values.
Strict correctly rounds the complete formula. Accelerated allows final nonzero finite error <=4 ULP, exact endpoint/classification/sign agreement and monotone nondecreasing results for fixed bounds. Strict fallback is reported when needed; the combined implementation must remain monotone and partition-independent.
These operations do not modify a color description or implement tone mapping.

## Execution and failures

For nonempty Q, read input[Q] and both bounds even at exact endpoints or NaN.
Empty Q reads no payload. Inherit exact demand/dirty mapping, typed/upstream
closures, immutable packed Region origins, arbitrary source strides, owner
lifetime, cache-off and capacity/work/stage/cancellation obligations from CRV-08.
Account certified numerical refinement and scratch growth; do not use a separately rounded pow/log pipeline as the strict oracle.
Invalid bounds use InvalidArgument/InvalidDomain; shape/dtype mismatch uses
TypeMismatch. Numeric special results succeed. Resource, backend, stale,
upstream and cancellation failures preserve the shared categories and atom scope.

## Public workflow and acceptance

lower=1, upper=16, input=[0.5,1,2,4,16,32] -> [-0.25,0,0.25,0.5,1,1.25].
Bind these ports through the public Compiler/ExecutionContext interface
using the selected operation key and check named values.
The maintained public workflow executes this fixture through Compiler/ExecutionContext.

Apply the shared independent oracle, endpoint/extreme/subnormal/NaN fixtures,
partial-request and dirty witnesses, strides, resource limits, cancellation,
cache-off and lifetime checks. Use certified whole-expression references, 4-ULP and cross-fallback monotonicity checks; forward/inverse rounded round trips are not generally bitwise identities.
The maintained target command and current validation boundary are linked below; the
specification remains Proposed.

## Maintained implementation and validation

The public entry point is `log2_shaper_node` in `photospider/numeric/shapers.hpp`. The log primitive uses certified whole-expression evaluation with a strict certified scalar fallback, preserves monotonicity and partition independence, and may return `ResourceExhausted` when 128..4096 refinement capacity is unresolved.
See [the CRV-08 family contract](CRV-08_shaper.md) and [the shaper workflow README](../../../../examples/numeric_workflow/README.md)
for the shared command, fixture and validation evidence. Native Clang 21
strict/Apple and Ubuntu WSL Clang 18 strict/AVX2 passed 4,196 independent
Fraction/directed-MPFR cases per profile. All five manual groups passed
all four profiles.
