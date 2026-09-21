---
spec_schema_version: 1
id: NUM-15A
parent_id: NUM-15
function: derivative_1d
proposed_operation_keys:
  - numeric.derivative_1d_strict
  - numeric.derivative_1d_accelerated_apple_silicon
  - numeric.derivative_1d_accelerated_x86_64
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

# NUM-15A: derivative_1d

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Differentiate one-dimensional samples using dynamic inputs `samples` [N] and
`step` [1], both Float32 or both Float64. Require 2<=N<=2^40 and finite nonzero
step, allowing negative step. Output values has shape [N], preserves dtype and
has empty facets. The starting coordinate does not affect this operation;
callers preserve or connect sampling-axis information separately. There are no
static numeric parameters or implicit casts/axis extraction.

## Difference formula and numeric semantics

Use first-order one-sided endpoints and central interior differences:

    D[0]   = (samples[1]-samples[0]) / step
    D[N-1] = (samples[N-1]-samples[N-2]) / step
    D[i]   = (samples[i+1]-samples[i-1]) / (2*step), 0<i<N-1

For N=2 both results use the same one-sided quotient. There is no edge-order
parameter or automatic higher-order stencil. For finite samples, evaluate the
whole rational quotient exactly and correctly round once to output dtype; accelerated uses the shared FP32-scaled bound. Neither an overflowing naive
subtraction nor 2*step intermediate changes the mathematical result.

Exact zero results are +0. Nonzero exact results that underflow retain their sign;
final overflow yields signed infinity successfully. Among required samples,
propagate the first NaN in ascending source index order, quieting it and preserving
payload/sign. Without source NaNs, same-sign infinities in the subtraction yield
fixed positive quiet NaN, otherwise subtraction/division determine the infinity
sign. Step is finite and nonzero, so it cannot introduce a numeric NaN after
successful validation. Fixed NaN bits follow [NUM-04](NUM-04_unary_contract.md).

## Dependencies, errors and invalidation

For any nonempty demand, Whole collects and validates complete samples and step
before callback. The callback rejects zero/nonfinite step with Domain/Run
InvalidArgument/InvalidDomain and InvalidSampleStep. A source failure may occur
before step validation. Empty reads nothing. All active input edits invalidate
all recorded output observations.

Only the stated two-point stencil enters each numerical result; an interior
center NaN still does not enter that result. Whole reads it and all other samples
for preparation and other complete outputs. Their upstream/typed failures may
fail the Run. Public output identity and numeric NaN priority are unchanged.

## Algorithms, resources and acceptance

Compute all N stencils with the existing exact quotient workspace, then publish
complete dense output. Full input collection, N*dtype_size output bytes and fixed
workspace are required even for sparse demand. A reusable rank-one coordinate
avoids per-read vector allocation. No per-output descriptors or dedup sets remain.
Cancellation/work checks cover reads, exact arithmetic, stores and publication.
Failed attempts release unpublished state/output; owner lifetime is unchanged.

Conceptual fixture: samples=[0,1,4], step=[1] -> [1,2,3]. This approximates the
derivative at endpoints; it does not claim the exact derivative of an unknown
underlying continuous function. For samples=[-MAX,0,MAX], step=[MAX], all outputs
are 1 despite overflowing naive intermediate differences. Verify with independent
exact rational arithmetic. Test negative step, N=2, step subnormals, NaN priority,
interior center NaN outside the requested stencil, infinities, signed zeros,
disjoint source-read witnesses, dirty inverse stencils, budgets and cancellation.

Deliver actual public WorkflowDocument execution and owner-lifetime checks when
implemented. Implementation evidence below records the checks actually run.


## Implementation and executable acceptance

All six formal calculus keys use Whole and preserve ExactCalculus arithmetic.
Current public workflow, independent Fraction oracle, layout/error/budget checks
and separate public/core timing are in
[NUM-15 Whole execution](../calculus-whole.md). Earlier regional WSL/installed
records are historical. Proposed status is unchanged.
