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
repository_commit: current working tree
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

Differentiate one-dimensional samples using two `Result` tensor inputs: samples
with `sample_shape()` [N] and step with `sample_shape()` [1]. Each input may use
any tensor member key. Both use Float32 or both Float64. Require 2<=N<=2^40 and
finite nonzero step, allowing negative step. The output port key is `values`;
its Result schema is `photospider.tensor` with tensor member `samples`, shape
[N], input dtype, and no facets. The
starting coordinate does not affect this operation;
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

Focused Result math coverage exercises public workflows, exact small examples,
negative step, N=2, exceptional values, budgets, pre-cancellation, and output
lifetime after context retirement.


## Implementation and executable acceptance

All six registered profile keys use Whole Result programs and the existing
`ExactCalculus` arithmetic. The manual public workflow binds Result sources
whose tensor storage references the fixture's backing Values. Nonempty sparse
requests still prepare all required inputs and publish the complete Whole
output with full tensor coverage. The query scopes the recorded dependency
observation and dirty mapping; it does not trim Result coverage. The fixture
checks that a center sample excluded from an interior stencil does not enter
that output's arithmetic, while Whole preparation reads and validates all active
sources. It also exercises
negative strides, unaligned and zero-stride input, caller and worker floating
environments during actual continuation polls, schema rejection, work limits,
cancellation, and output lifetime after context retirement.

The independent oracle retains 1,810 Fraction reference cases. Strict acceptance
is bit-exact. Apple acceptance uses the shared FP32-scaled accelerated bound and
includes a verified one-ULP difference. The root behavior test and installed
consumer compile the manual workflow with `-fno-fast-math -frounding-math
-ffp-contract=off`. Commands and current execution details are in
[NUM-15 Whole execution](../calculus-whole.md), and the runnable workflow is
listed in [the numeric workflow guide](../../../../examples/numeric_workflow/README.md#discrete-derivatives-and-cumulative-integration-num-15).
Proposed status is unchanged.
