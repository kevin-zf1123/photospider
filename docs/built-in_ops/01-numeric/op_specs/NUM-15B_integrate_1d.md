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

Compute cumulative trapezoidal integration from three `Result` tensor inputs in
port order: samples with `sample_shape()` [N], step [1], and initial [1]. Each
input may use any tensor member key; all share Float32 or Float64 dtype. Require
1<=N<=2^40. The output port key is `values`; its Result schema is
`photospider.tensor` with tensor member `samples`, shape [N], input dtype, and
no facets. There are no static numeric parameters or
implicit casts/axis extraction.

values[0] = initial
    values[i] = initial + step * sum_{k=0..i-1}(samples[k]+samples[k+1])/2

Output0 copies initial raw bits, including sNaN/Inf/signed zero. When N=1,
static runtime projection excludes samples and step completely, including their
failed producers, while all port metadata is checked. For N>1, every nonempty
demand reads/validates full samples, step and initial; step must be finite nonzero,
including when only output0 is projected. Negative step remains valid.

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

For N>1 Whole input preparation precedes callback step validation. Invalid step
returns Domain/Run InvalidArgument/InvalidDomain with InvalidSampleStep; source
failure may occur first, and initial NaN cannot suppress required source failure.
For N=1 only initial is active. Empty reads nothing. Any active input edit
invalidates all recorded output observations, including index0 for N>1.

## Algorithms, resources and publication

Keep the exact unweighted source prefix, first/last samples and source special
classifications. A separate ratio workspace forms2*sum-first-last, multiplies the
exact step and adds aligned initial, with final scale-2149 rounding. The prefix
survives every conversion; public rounded outputs never become state. Compute
all N outputs before consumer projection. Fixed ExactCalculus state, complete
output and full collected active inputs replace regional windows, point plans
and per-output associations. No persistent checkpoint is retained.

Source/control strides and offsets remain valid. Work/cancellation checks cover
reads, exact accumulation/refinement, stores and publication. Errors retain their
categories and release all unpublished memory. N=1 copies initial without
constructing numerical state, while registered workspace admission remains a
conservative common bound. Metadata violations remain schema errors.

## Acceptance and implementation status

Conceptual fixture: samples=[0,1,2], step=[1], initial=[0] -> [0,0.5,2].
For constant samples=[2,2,2], step=[0.5], initial=[3], result is [3,4,5].
Use independent exact weighted-prefix arithmetic and explicit nonfinite tables.
Test negative step, N=1, raw sNaN/negative-zero initial at output 0, invalid step
ignored only for N=1 and rejected for all N>1 demands, NaN source priority,
infinity cancellation, subnormals and finite cancellation after large areas.

Focused coverage exercises public workflows, exact weighted-prefix arithmetic,
raw output-zero copying, N=1, invalid step validation, NaN priority, infinities,
pre-cancellation, resource limits and output lifetime. For N=1, the tests connect
deliberately failing producers to samples and step; neither producer starts, the
initial sNaN bits are copied, and dependency observations name only initial.
Empty demand also starts no producer. For N>1, an output-zero request starts
required producers and propagates their failure; statically excluding step still
retains dtype validation. This trapezoidal rule approximates an underlying
function's integral; exact rounding concerns the stated discrete formula.

## Implementation and executable acceptance

All six registered profile keys execute Whole Result programs and preserve the
`ExactCalculus` weighted-prefix arithmetic. For N=1, static specialization
selects only input 2 (initial); samples and step metadata remain checked, but
runtime support excludes them and their failed producers remain unstarted. Empty
demand starts no producer and publishes empty tensor coverage. For N>1, each
nonempty query prepares all active inputs even when it requests output 0, then
publishes the complete output with full tensor coverage. The query scopes the
recorded dependency observation and dirty mapping; it does not trim Result
coverage. The manual workflow verifies failure ordering, raw output-zero
copying, sparse requests, negative and zero strides, unaligned
storage, caller and worker floating environments on actual continuation polls,
resource limits, cancellation, and Result/window lifetime through Root release.

The independent oracle retains 1,810 Fraction reference cases. Strict acceptance
is bit-exact. Apple acceptance follows the shared FP32-scaled bound and includes
a verified one-ULP difference; it is not a bit-exact claim. The root behavior
test and installed consumer compile the same manual workflow with
`-fno-fast-math -frounding-math -ffp-contract=off`. Commands and current
execution details are in [NUM-15 Whole execution](../calculus-whole.md), and the
runnable workflow is listed in [the numeric workflow guide](../../../../examples/numeric_workflow/README.md#discrete-derivatives-and-cumulative-integration-num-15).
