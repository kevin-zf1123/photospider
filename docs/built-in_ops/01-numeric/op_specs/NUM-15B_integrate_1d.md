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
implementation_status: implemented
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-15B: integrate_1d

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

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
only once to output dtype. Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound.
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
integral; exact rounding concerns the stated discrete formula. Implementation evidence below records the checks actually run.


## Implementation and executable acceptance

Six suffixed calculus keys are registered in `numeric_calculus.cpp`; public
helpers are `derivative_1d_node` and `integrate_1d_node` in
`photospider/numeric/calculus.hpp`. Regional execution first requests step as
Control/Validation for dependent observations. Integration always requests
initial as Data/Validation and does not request step or samples for index zero.
Positive outputs retain their actual sample support even when initial is NaN.

Derivative uses exact integer differences and a positive step magnitude,
including the exact interior factor two; the sign includes step's sign. The
4352-bit ratio scratch covers all required alignment/refinement. Integration
retains an exact unweighted sum, source classification, and first/last samples.
A separate conversion workspace forms `2*sum-first-last`, multiplies by the
full exact step coefficient, adds aligned initial, then rounds with scale
-2149. Its numerator needs at most 4237 bits and rounding scratch at most 4238.
Initial-only output is a raw copy, while positive exact-zero outputs are +0.

Integration transports at most 64 samples per window and scans to the largest
requested index once per regional invocation. Every window retains complete
per-observation associations; dense boundary requests can incur quadratic
metadata work. State, output plan and metadata are accounted; work/capacity/stage
limits reject excessive requests. There are no persistent checkpoints, and
separate executions or execute_atoms observations can repeat numeric work.

The manual `photospider_numeric_calculus` target, `calculus.cpp`, and independent
`calculus_oracle.py` are documented in
[the workflow example](../../../../examples/numeric_workflow/README.md).
Local Clang 21 strict/Apple and Ubuntu WSL Clang 18 strict/AVX2 passed 1,810
independent Fraction/raw-bit cases per profile on 2026-09-19 and the manual
workflow checks: sparse stencil/center-NaN exclusion, raw initial-only output,
invalid-step Atom isolation and producer order, required upstream failure after
initial NaN, all-port strides/fenv, Empty/schema, WorkLimit/cancellation and
release. A 4096-input integral with four sparse outputs uses 64 windows and
exact sample/step/initial dirty support. Installed strict/Apple consumers,
focused compiler unit, formatting/lint and scoped math/entry reviews passed.
The manual target has no CTest/integration registration; specification status
remains Proposed independently of implementation evidence.
