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

Read witnesses must show initial-only for N=1 and complete active inputs for
N>1. Exercise disjoint outputs, dynamic step/initial invalidation, source
strides, low work/capacity limits, checkpoint partition invariance, cancellation,
cache-off and result lifetime through actual public WorkflowDocument execution
when implemented. This trapezoidal rule approximates an underlying function's
integral; exact rounding concerns the stated discrete formula. Implementation evidence below records the checks actually run.


## Implementation and executable acceptance

All formal calculus profiles execute Whole. Mathematical weighted-prefix and
raw output0 rules are unchanged. See [NUM-15 Whole execution](../calculus-whole.md)
for current public workflow, oracle, resource checks and profiling. Older regional
WSL/installed records predate Whole.
