---
spec_schema_version: 1
id: CRV-05A
parent_id: CRV-05
function: apply_lut1d
operation_family: curve.apply_lut1d
proposed_operation_keys:
  - curve.apply_lut1d_strict
  - curve.apply_lut1d_accelerated_apple_silicon
  - curve.apply_lut1d_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
verification_status: manual_public_workflows_and_independent_oracle
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-05A: apply_lut1d

Inherit the [NUM baseline](NUM_common_contract.md) and the explicitly cited
[CRV-01A linear contract](CRV-01A_interpolate_linear.md) for unchanged rounding,
resources, host errors, ownership and acceptance. The uniform axis, arbitrary
input shape and singleton-table rules below take precedence. Clarification is
complete; specification status remains Proposed while the public operation is
implemented.

## Confirmed purpose

Map each numeric input element through one scalar table[L], preserving the
input array shape in output values. This is independent of the per-channel
multi-table operation and of a scalar-to-vector color ramp.

## Confirmed dynamic interface

Use required dynamic inputs input, table[L] and axis[3], in that order. Input is
a generic numeric array of arbitrary supported rank. Axis carries start/end/step
and directly connects to CRV-04's named axis output; table connects to values.
The exact sample-grid definition uses start/end and table length L. The supplied
step is checked for consistency, not repeatedly accumulated to create knots.
Output values preserves input shape. Detailed axis and dtype rules follow.

## Confirmed interpolation method

Use fixed linear interpolation between adjacent samples in table order, with
their actual reconstructed coordinates. At an exact sample-coordinate hit,
select the corresponding table value directly. No method parameter or implicit
nearest/PCHIP behavior is provided. Source PCHIP can be baked into this discrete
table, but the later LUT application still uses its own piecewise-linear function.

## Confirmed multi-sample axis validity

For L>=2, allow ascending or descending start/end, but require the complete
reconstructed Float64 sample grid to be strictly ordered with no repeated
coordinates. Validate the entire grid for any nonempty application request.
Each coordinate is reconstructed with the sampler's direct endpoint-weighted
RN64 rule, with exact endpoint copies, not accumulated step additions.
Axis.step must match the correctly rounded endpoint-derived step. Invalid
ordering, repeated sample coordinates or inconsistent step fails the observation;
the operator does not merge table entries or silently repair the axis.

## Confirmed domain behavior

Static out_of_domain is reject/clamp/linear_extrapolate, default reject.
Compare the input with the actual minimum/maximum axis coordinates, accounting
for descending tables. Reject fails outside that closed domain. Clamp selects
the nearest numerical-domain endpoint's table value. Linear extrapolation uses
the adjacent pair at that domain end, retaining its actual signed coordinate
spacing. Array index zero need not be the minimum-coordinate endpoint.

For L=1, require axis=[start,start,+0]. Reject accepts only input numerically
equal to start; clamp and linear_extrapolate both return the sole table value,
treating the extrapolated slope as zero. Every requested input is still read and
validated; a one-entry table does not erase that dependency or its errors.

## Confirmed dtype mapping

Input and table independently accept Float32/Float64 and may mix dtypes. Axis
is always Float64 [3]. Output dtype statically selects Float32/Float64, with
the constructor default matching input dtype. Direct nodes supply the selected
dtype explicitly. Neither table dtype nor axis dtype forces an implicit output
promotion. Integer numeric inputs require explicit conversion before application.

## Confirmed numerical quality and table demand

Inherit CRV-01A linear's numerical and local-read rules: strict and both CPU
accelerated versions correctly round the complete selected linear interpolation
or extrapolation expression once to output dtype, with identical bits. Actual
requested input values, selected table entries and final output must be finite.
Nonfinite demanded data or final overflow fails the affected observation.

An exact knot hit, clamp or singleton-table constant selects one table entry;
an interpolation/extrapolation segment reads both adjacent entries. Other table
values are neither read nor numerically validated. Exact selection correctly
converts the source value and preserves its zero sign. Other exact zero results
are -0 only when both selected endpoints are -0, otherwise +0; nonzero underflow
retains its mathematical sign. Global axis validity remains an independent
obligation even for a one-entry table read.

## Complete descriptors and axis formula

Require 1<=L<=1048576. Input has rank 1..8, positive extents and logical element
count <=2^40. Table is rank-1 [L]; axis is Float64 [3]. Output values preserves
input shape, has the chosen floating dtype and empty facets. No input semantic
facet, color role or unit is inferred or propagated to the generic output;
actually read recognized typed inputs retain their separate validation closure.

Required static String dtype is float32/float64 and out_of_domain is
reject/clamp/linear_extrapolate. The constructor writes input's dtype and reject
by default; direct nodes supply both. No interpolation-method, axis-shape or
runtime output-shape parameter is introduced. Static metadata checks cover every
declared edge independently of requested data and infer output without evaluation.

Let a=axis[0], b=axis[1], d=axis[2]. All must be finite. For L=1 require b
to preserve a's bit pattern and d=+0; the singleton coordinate is a. For L>=2:

    expected_step = RN64((exact(b)-exact(a))/(L-1))
    x[0] = a; x[L-1] = b
    x[j] = RN64(((L-1-j)*exact(a)+j*exact(b))/(L-1)), 0<j<L-1

Require a!=b, finite nonzero expected_step with direction b-a, and d bitwise
equal to expected_step. Validate x[j]<x[j+1] for ascending axes, or > for
descending axes, across the complete grid. Exact intermediate arithmetic avoids
overflow in the coordinate formula; a nonfinite derived step is invalid axis.
Signed-zero endpoints retain their supplied bits, but numerical duplicate
coordinates are invalid. The full validation obligation consumes work even if
the requested query hits an endpoint.

For a non-knot query q inside the domain, find its unique adjacent bracketing
pair j,j+1 in table order. Compute the exact mathematical value

    ((x[j+1]-q)*table[j] + (q-x[j])*table[j+1])/(x[j+1]-x[j])

with one final destination rounding. For extrapolation use the nearest domain
end's adjacent pair in the same formula, including negative denominators for a
descending table. Endpoint/clamp/singleton paths apply their exact selection
rules instead. The specification interpolates between rounded Float64 grid
coordinates, not unrounded ideal positions or a rounded fractional index.

## Exact demand and invalidation

For nonempty output Q, read the entire three-component axis as shared Control/
Validation. Complete grid validation needs computed coordinates, not table-value
reads. Input Control support is exactly Q. Read each requested input even if
L=1 or the selected table values are equal. Validate axis before query-dependent
table lookup; invalid input or reject-domain query adds no table Data demand.

Table Data is the exact union of selected singleton/pair indices for requested
input coordinates. Do not read remote table values for a convenient full-table
validation pass. Empty Q reads no axis/input/table payload. Add typed Validation
support separately and preserve any genuinely required upstream Whole execution.
No SIMD tail or source bounding gap is authorized by the lookup implementation.

Axis changes invalidate/replan all observations retaining its witness. Changed
input at coordinate q invalidates values[q]; changed table[j] invalidates only
observations whose lookup singleton/pair contains j, plus typed validation.
Cache an index/lookup only with its axis/input versions and static geometry.
Table size/dtype, input metadata, output dtype, policy and numeric profile remain
in plan identity. A coincident unchanged numerical output does not erase a read.

Return immutable owned packed fragments with the same global shape coordinates
as Q and correct Region/storage origins. Accept valid immutable arbitrary strides,
offsets and unaligned data at all ports. No whole-input output allocation, writable
table alias or implicit zero gaps are required. Published owners survive context
destruction until final release; cache-off preserves active ownership.

## Algorithm, resource and failure contract

A reference streams the L reconstructed coordinates for validation, optionally
retaining an admitted 8L-byte Float64 grid (at most 8 MiB). Binary search either
uses that grid or reconstructs probed coordinates exactly. For M requested
elements, reference work is O(L+M log L), with constant-time L=1 lookup, plus
coordinate/interpolation arithmetic and typed validation. Coordinate generation
does not require reading every table value. Fast index estimates must be verified
against exact rounded knots and corrected without weakening the bracketing rule.

Output payload is b*M bytes. Account grid/index and dedup metadata, selected table
and input owners/windows, every exact-arithmetic limb, validation witnesses,
scratch and output capacity including growth overlap. Global axis validation
may exhaust work even for tiny Q; fail explicitly rather than skip it. Optional
immutable validated-grid caches require witnessed axis versions. Use host workers,
poll cancellation at least every 64 coordinates/lookups and during long arithmetic,
and preserve the floating environment. Do not add unbudgeted disk or private pools.

| Trigger | Phase and Status |
| --- | --- |
| Missing/malformed dtype or domain policy | Compile/preflight; InvalidArgument / InvalidDomain |
| Unsupported input/table/axis dtype or shape, size cap | Compile/preflight; TypeMismatch |
| Nonfinite/inconsistent axis or duplicate reconstructed coordinates | Evaluation; OperationFailed / InvalidDomain |
| Nonfinite demanded input/table, or reject-domain query | Evaluation; OperationFailed / InvalidDomain |
| Actual destination conversion overflow | Evaluation; OperationFailed / ArithmeticOverflow |
| Host budgets, unavailable platform, typed/upstream failure, cancellation/stale | Preserve baseline Status, producing identity and scope |

Each numeric failure identifies its dependent output Atom; whole-axis validation
does not retroactively revoke independent completed outcomes. Diagnostics identify
axis component/knot index or query/table coordinate when available. Failed
observations publish no partial successful Value. Ordinary fail-fast and eligible
per-atom execution retain their distinct existing host contracts.

## Acceptance and current implementation

Conceptual fixture: table=[0,0.25,1], axis=[0,1,0.5], input=[0,0.25,0.5,1]
-> values=[0,0.125,0.25,1]. Reversing table and using axis=[1,0,-0.5] gives
the same function. A singleton table=[7], axis=[2,2,+0] accepts input=2 under
reject and maps any finite input to 7 under clamp/linear_extrapolate.

Use independent exact coordinate reconstruction and rational interpolation,
with bit inspection for axis-step equality, knots, zero signs and output rounding.
Test all domain modes, descending axes, mismatched step, collapsed coordinates,
L limits, source-type mixing, endpoint narrowing overflow versus finite interior
results, huge cancellation and subnormal queries/results. Three CPU profiles
must have identical logical values and numerical failure classifications.

Public implementation delivery binds input/table/axis, supplies dtype/policy,
and requests named values through Compiler/ExecutionContext with actual run
commands. Verify exact table/input read sets, unused nonfinite table entries,
global-axis failures, typed closures, dirty mapping, source strides, cache-off,
resource/cancellation and post-context owner lifetime. Also connect each applicable
CRV-04 scalar template directly to table/axis; its allowed constant/repeated grid
may still be rejected by this consumer's stricter axis validity.

Current [field.apply_lut_1d](../../../../plugins/ops/01-numeric/field_apply_lut_1d.cpp)
instead takes a rank-2 field and same-dtype table, static increasing domain and
reject/clip policy, validating the whole table under Whole execution. Current
[lut.apply_1d](../../../../plugins/ops/01-numeric/lut_apply_1d.cpp) uses typed
Float32 input/table with semantic sample origin/step. Neither is this generic
three-input dynamic-axis, versioned contract. No runtime tests of the new keys
are inferred from those legacy operations. The implemented target is validated
as recorded below.

### Maintained implementation and verification

`plugins/ops/01-numeric/lut1d_application.cpp` registers the six scalar/channels
profile keys. `UniformAxis` validates all three raw axis values, exact step and
the entire directly reconstructed grid, retaining an admitted 8L-byte index.
Each requested query is classified independently before local table Data is
requested. Descending pairs reorder x and y together before using ExactCurve's
positive-denominator linear formula. Every profile correctly rounds the full
formula and preserves the specified signed-zero/finite classifications.

`apply_lut1d_node` in `photospider/numeric/lut1d.hpp` takes the three dynamic
references and an explicit input dtype hint for its default output dtype.
Compiler checks actual edge types and shapes. The complete public fixtures and
commands are in the [numeric workflow README](../../../../examples/numeric_workflow/README.md#lut1d-application-crv-05).
The three-poll Needs plus final publication preserve exact scalar Atom support;
Image input Validation closes full channels independently of query Control.
Each explicit per-cell Need reserves 4096+16384*M metadata bytes, in addition
to grid, point, output and exact scratch owners. Default work/metadata limits do
not promise every dense request will complete. Global grid work is required
even for an endpoint query; it can exhaust caller-selected budgets.

On 2026-09-20 native Apple M5 Clang 21 strict/Apple and Ubuntu WSL i9-12900
Clang 18.1.3 strict/AVX2 passed six manual groups and 1416 independent Fraction
grid/linear cases per profile. They exercise ascending/descending and singleton
axes, finite extreme cancellation, underflow/zero signs, mismatched and collapsed
axes, mixed dtypes, typed support, rank-8 error Atoms, cache reselection,
strides/fenv, arithmetic and axis-loop cancellation, state/work/stage limits and
owner release. The public constant-node fixtures validate a full L=1048576 grid
while reading one scalar-backed table entry, and one component of a 2^39-channel
logical table. All six CRV-04 templates are connected directly to scalar/channels
consumers and their discrete expected values checked. Installed 0.15 consumers,
focused compiler unit, formatting/lint and independent math/entry reviews passed.
The manual executable is excluded from default builds and CTest/integration;
WSL is used for correctness only.

- [Family decisions](CRV-05_apply_lut1d.md).
- [Baking templates](CRV-04_bake_lut1d.md).
- [Operator template](../../00-foundation/spec-template.md).
