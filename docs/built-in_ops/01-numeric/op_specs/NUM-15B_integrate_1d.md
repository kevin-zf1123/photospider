---
spec_schema_version: 1
id: NUM-15B
parent_id: NUM-15
function: integrate_1d
proposed_operation_keys:
  - numeric.integrate_1d_strict
  - numeric.integrate_1d_accelerated_apple_silicon
  - numeric.integrate_1d_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-15B: integrate_1d

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute cumulative trapezoidal integration of dynamic samples [N], step [1]
and initial [1], in that port order, all with the same Float32/Float64 dtype.
Require 1<=N<=2^40. Output values retains shape [N] and dtype, with empty facets.
There are no static numeric parameters or implicit casts/axis extraction.

    values[0] = initial
    values[i] = initial + step * sum_{k=0..i-1}(samples[k]+samples[k+1])/2

For output 0, read only initial and copy its exact bits, including sNaN, infinity
and signed zero. Do not read samples values or step. This also defines N=1.
Only requests containing i>0 require step to be finite and nonzero, allowing
negative step, and source Data support samples[0..i] inclusive. Input metadata
and all port dtype/shape constraints are still checked at compile/preflight.

## Whole-formula quality and special values

At i>0, evaluate the entire initial-plus-area expression mathematically, rounding
only once to output dtype. All three CPU versions are bitwise equivalent.
For finite sources, an equivalent exact expression is

    initial + step/2 * (samples[0] + 2*sum(samples[1:i]) + samples[i])

Do not round trapezoids, prefixes or the area before adding initial. Unrequested
intermediate output overflows do not fail a later representable requested value.
Exact zero at i>0 is +0; nonzero exact underflow retains its sign. Final overflow
yields correctly signed infinity as a successful numeric result.

For i>0, source NaN priority is initial first, then samples by increasing index
within the contributing prefix. Quiet it preserving payload/sign. With no NaN,
opposite signed sample infinities make the trapezoidal area undefined and yield
fixed positive quiet NaN. Otherwise the sample infinity sign is multiplied by
step's sign, then combined with initial: opposite infinities yield canonical NaN,
one infinity sign determines the result. Finite weighted sums and products stay
exact until final output classification. Output 0 retains its separate raw-copy
contract; a generated prior-output NaN is never treated as a new source operand.

Fixed NaN patterns and floating-environment preservation follow
[NUM-04](NUM-04_unary_contract.md); output 0 retains its explicit raw-copy exception.

## Validation, dependence and invalidation

For any requested positive index, validate step before dependent sample reads.
Zero/nonfinite step fails with InvalidArgument, FailureReason::InvalidDomain and
diagnostic InvalidSampleStep, with Atom attribution for each dependent positive
output observation. An independently completed output[0] remains successful.
For a request solely at index 0, step is not read
or numerically validated. Initial is read for every nonempty request, and all
actually contributing samples are read even if a source NaN fixes the result.
Empty Q reads no source values.

For positive requested indices, source Data is the exact union of prefixes
[0,i+1); scan only up to the largest requested i. Add recognized typed-input
Validation closure separately. Retain step Control/validation and initial/sample
Data witnesses. Initial changes affect all outputs; step changes affect indices
>0 only. A sample change at j affects positive outputs i>=max(1,j), plus any
retained validation effects. Metadata/profile and witnessed source versions form
cache identity. No unread source failure is requested on the output-0 path.

## Algorithms, resources and publication

Use exact weighted prefix state or certified equivalent, retaining infinity flags
and earliest actual source NaN. Work is proportional to the largest required
sample prefix plus exact arithmetic and requested output conversion. Bound active
state, account every accumulator/limb, source owner/window, dependency metadata,
output fragment and scratch buffer. Rounded public outputs are not continuation
state. Optional exact checkpoints are immutable, versioned and host-accounted;
cache-off changes neither values nor ownership.

Read arbitrary valid immutable strides/offsets. Return owned packed requested
fragments with correct global origins; do not allocate a whole cumulative array
for a sparse request. Poll cancellation per scan block, at least every 4096 source
samples and during extended arithmetic. Resource exhaustion, upstream or typed
failure keeps its Status; do not publish a partial failed observation. Published
owners survive context destruction until final release. Invalid port metadata,
N or dtype mismatches fail compile/preflight.

## Acceptance and implementation status

Conceptual fixture: samples=[0,1,2], step=[1], initial=[0] -> [0,0.5,2].
For constant samples=[2,2,2], step=[0.5], initial=[3], result is [3,4,5].
Use independent exact weighted-prefix arithmetic and explicit nonfinite tables.
Test negative step, N=1, raw sNaN/negative-zero initial at output 0, invalid step
ignored by output 0 but rejected by positive requests, NaN source priority,
infinity cancellation, subnormals and finite cancellation after large areas.

Read witnesses must show initial-only for index 0 and exact prefixes for positive
requests. Exercise disjoint outputs, dynamic step/initial invalidation, source
strides, low work/capacity limits, checkpoint partition invariance, cancellation,
cache-off and result lifetime through actual public WorkflowDocument execution
when implemented. This trapezoidal rule approximates an underlying function's
integral; exact rounding concerns the stated discrete formula. No versioned
runtime implementation or tests are claimed by this specification.
