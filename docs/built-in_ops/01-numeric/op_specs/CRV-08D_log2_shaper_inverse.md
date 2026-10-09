---
spec_schema_version: 1
id: CRV-08D
parent_id: CRV-08
function: log2_shaper_inverse
proposed_operation_keys:
  - curve.log2_shaper_inverse_strict
  - curve.log2_shaper_inverse_accelerated_apple_silicon
  - curve.log2_shaper_inverse_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-08D: log2_shaper_inverse

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and formula

Ordered inputs are Results named input, lower and upper. Each has one tensor
member under any schema id/version/key; use its full `sample_shape()`, including
batch axes. `input` is Float32/64, rank 1..8, positive extents and at most 2^40
elements. Bounds are same-dtype scalar tensors of shape [1], finite and satisfy
0<lower<upper. Output port `values` is an immutable Result using
`photospider.tensor` v1/member `samples`, preserving input shape/dtype with
empty facets. No implicit clipping occurs.
The mathematical formula is lower*(upper/lower)^input, with whole-expression final rounding.
The independent operation key selects the CPU profile; there is no additional static mode parameter.

Inherit the [complete shaper contract](CRV-08_shaper.md) for exact endpoints,
zero signs, NaN quieting/payloads, infinity/domain extensions and overflow as
successful IEEE results. Bound validation always precedes source special values.
Strict correctly rounds the complete formula. Accelerated allows final nonzero finite error <=4 ULP, exact endpoint/classification/sign agreement and monotone nondecreasing results for fixed bounds. The strict path is retained when needed; Whole counters are unavailable; the combined implementation must remain monotone and partition-independent.
These operations do not modify a color description or implement tone mapping.

## Execution and failures

Nonempty Whole requests all three inputs with Data, Validation and Descriptor
(role 13), including complete typed/upstream validation. The Result program reads
authorized windows directly; it does not collect or copy the full input through
Value. Empty reads no payload. Inherit full-output allocation, full dirty scope,
sparse public delivery, arbitrary input strides, ownership, budgets and
cancellation from CRV-08.
Account certified numerical refinement and scratch growth; do not use a separately rounded pow/log pipeline as the strict oracle.
Invalid bounds use InvalidArgument/InvalidDomain; shape/dtype mismatch uses
TypeMismatch. Numeric special results succeed. Resource, backend, stale,
upstream and cancellation failures preserve the shared categories; numeric failures have Run scope.

## Public workflow and acceptance

lower=1, upper=16, input=[-0.25,0,0.25,0.5,1,1.25] -> [0.5,1,2,4,16,32].
Bind these ports through the public Compiler/ExecutionContext interface
using the selected operation key and check named values.
The maintained public workflow executes this fixture through Compiler/ExecutionContext.

Apply the shared independent oracle, endpoint/extreme/subnormal/NaN fixtures,
partial-request and dirty witnesses, strides, resource limits, cancellation,
cache-off and lifetime checks. Use certified whole-expression references, 4-ULP and cross-fallback monotonicity checks; forward/inverse rounded round trips are not generally bitwise identities.
The maintained target command and current validation boundary are linked below; the
specification remains Proposed.

## Maintained implementation and validation

The public entry point is `log2_shaper_inverse_node` in
`photospider/ops/numeric/shapers.hpp`. The log primitive uses certified
whole-expression evaluation with a strict certified scalar fallback, preserves
monotonicity and partition independence, and may return `ResourceExhausted` when
128..4096 refinement capacity is unresolved. See [the CRV-08 family contract](CRV-08_shaper.md)
and [the shaper workflow README](../../../../examples/numeric_workflow/README.md)
for the shared command, fixture and validation evidence. The installed consumer passes 1/1, including public log
composition and output lifetime checks. x86 numerical, GPU, maximum-size and
performance validation are not covered.
