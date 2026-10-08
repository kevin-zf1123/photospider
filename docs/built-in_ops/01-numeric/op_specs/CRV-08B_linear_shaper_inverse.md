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
implementation_status: implemented
verification_status: focused_result_validation_passed
clarification_status: complete
repository_branch: ops-specs
repository_commit: current working tree
---

# CRV-08B: linear_shaper_inverse

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and formula

Ordered inputs are Results named input, lower and upper. Each has one tensor
member under any schema id/version/key; use its full `sample_shape()`, including
batch axes. `input` is Float32/64, rank 1..8, positive extents and at most 2^40
elements. Bounds are same-dtype scalar tensors of shape [1], finite and ordered
lower<upper. Output port `values` is an immutable Result using
`photospider.tensor` v1/member `samples`, preserving input shape/dtype with
empty facets. No implicit clipping occurs.
The mathematical formula is lower+input*(upper-lower), with whole-expression final rounding.
This named authoring template expands Result-based remap_range and explicit
broadcast/constant nodes. Static profile is strict/apple_silicon/x86_64, default
strict; it does not register a new primitive.
The mandatory inverse lower<upper guard is the scalar remap_range composition specified in CRV-08; do not inherit permissive target-bound order unchecked.

Inherit the [complete shaper contract](CRV-08_shaper.md) for exact endpoints,
zero signs, NaN quieting/payloads, infinity/domain extensions and overflow as
successful IEEE results. Bound validation always precedes source special values.
Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound.
These operations do not modify a color description or implement tone mapping.

## Execution and failures

Nonempty Whole requests all three inputs with Data, Validation and Descriptor
(role 13), including complete typed/upstream validation. The remap Result program
reads authorized windows directly; it does not collect or copy the complete
input through Value. Empty reads no payload. Inherit full-output allocation,
full dirty scope, sparse public delivery, arbitrary input strides, ownership,
budgets and cancellation from CRV-08.
Account all internal guard/broadcast/remap state, not only the exported output.
Invalid bounds use InvalidArgument/InvalidDomain; shape/dtype mismatch uses
TypeMismatch. Numeric special results succeed. Resource, backend, stale,
upstream and cancellation failures preserve the shared categories; numeric failures have Run scope.

## Public workflow and acceptance

lower=-2, upper=2, input=[0,0.5,1,1.5] -> [-2,0,2,4].
Bind these ports through the public Compiler/ExecutionContext interface
after template expansion and check named values.
The maintained public workflow executes this fixture through Compiler/ExecutionContext.

Apply the shared independent oracle, endpoint/extreme/subnormal/NaN fixtures,
partial-request and dirty witnesses, strides, resource limits, cancellation,
cache-off and lifetime checks. Use an exact rational whole-formula oracle; inverse acceptance includes lower>=upper rejection.
The maintained target command and current validation boundary are linked below; the
specification remains Proposed.

## Maintained implementation and validation

The public entry point is `linear_shaper_inverse` in
`photospider/numeric/shapers.hpp`. This linear helper expands to existing Result
remap/constant nodes and is not a linear primitive.
See [the CRV-08 family contract](CRV-08_shaper.md) and [the shaper workflow README](../../../../examples/numeric_workflow/README.md)
for the shared command, fixture and validation evidence. The current Result
focused CTest passes 1/1; six manual groups pass under Strict and the local Apple
profile, and the Result probe passes 4,196 Fraction/directed-MPFR 4.2.2 cases
under each profile. The installed consumer passes 1/1, including public
composition and output lifetime checks. x86 numerical, GPU, maximum-size and
performance validation are not covered.
