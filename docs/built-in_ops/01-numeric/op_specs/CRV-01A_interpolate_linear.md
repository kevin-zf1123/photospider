---
spec_schema_version: 1
id: CRV-01A
parent_id: CRV-01
function: interpolate_linear
operation_family: curve.interpolate_linear
proposed_operation_keys:
  - curve.interpolate_linear_strict
  - curve.interpolate_linear_accelerated_apple_silicon
  - curve.interpolate_linear_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
implementation_branch: numeric-optimize
implementation_base_commit: 3d35f5eb
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-01A: interpolate_linear

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for registration, descriptor
inference, platform availability, floating environment, resource accounting,
error attribution and implementation acceptance. The explicit finite-value,
input-layout and query-dependent rules below take precedence. Specification
clarification is complete; status remains Proposed and implementation is separate.

## Confirmed purpose and interface

Evaluate a single piecewise-linear function through supplied known points at
explicit dynamic query positions. Inputs in order are x[K], y[K], query[N];
output values[N] evaluates the function at each query position. The mathematical
curve passes through the supplied (x,y) points. Input descriptors determine K
and N; query values are runtime data rather than a static interval parameter.

Single-function and multiple-function interpolation are separate implementations.
This operation has no additional C axis. Linear and PCHIP are independently
named operations with separate specifications; this operation has no method
switch. For a selected segment with distinct endpoint abscissas x[j], x[j+1],
the linear mathematical expression is

    t = (query[i] - x[j]) / (x[j+1] - x[j])
    values[i] = (1-t)*y[j] + t*y[j+1]

## Confirmed knot and query ordering

Require 2<=K<=65536, 1<=N<=2^40 and every x value finite, with x[j]<x[j+1]. Do not automatically
sort knots, merge duplicates or accept a decreasing x array. Query positions may
be unsorted and may repeat; preserve their original order in values. No duplicate
query rejection or automatic resampling is introduced.

An analytic fixture is x=[0,1,3], y=[0,2,4], query=[2,0.5,2], giving
values=[3,1,3]. It covers irregular knot spacing, out-of-order queries and a
repeated query. Global knot validation follows the requested-data section below.

## Confirmed dtype contract

Each of x, y and query independently accepts Float32 or Float64. They may have
different floating dtypes. Input values denote their exact binary numerical
values; Float32 widening to Float64 is exact. No input integer dtype or implicit
integer conversion is provided; connect an explicit cast where needed.

Output values selects Float32 or Float64 through a static dtype parameter,
defaulting to Float64. Conversion/rounding boundaries and special-value handling
are specified separately rather than inferred from native mixed-type arithmetic.

All three required ports are generic rank-1 Values, with x/y sharing K and
query defining N. Accept legal immutable zero/negative strides, nonzero offsets
and unaligned storage. Output values is a generic Value with shape [N], chosen
dtype and empty facets; no axis output or implicit physical/color units are
created. Query values already provide the requested independent coordinates.

| Static parameter | Type and domain | Constructor default |
| --- | --- | --- |
| dtype | String float32 or float64 | float64 |
| out_of_domain | String reject, clamp or linear_extrapolate | reject |

Direct WorkflowDocument nodes supply both parameters explicitly. K/N derive
from descriptors rather than count parameters. Missing/extra ports, unsupported
dtype, wrong rank, x/y length mismatch and size excess fail compile/preflight.

## Confirmed domain policies

Static out_of_domain selects reject, clamp or linear_extrapolate, default reject.
For a query below the first or above the last knot:

- reject fails the affected observation.
- clamp returns the nearest endpoint's y value with the selected output conversion.
- linear_extrapolate extends the first or last segment's straight line using
  that segment's endpoints in the same linear expression.

For x=[0,1], y=[0,2], query=[2], the respective outcomes are failure, [2], [4].
The finite endpoint interval includes both endpoints. No automatic clipping of
the y result or implicit change of the query position is introduced.

## Confirmed numerical versions

Use the three independently named CPU keys in front matter. Strict evaluates
the entire mathematical linear interpolation/extrapolation expression, treating
input floats as exact binary rationals, and rounds only the final result directly
to the output dtype using nearest/ties-to-even. No intermediate rounded t,
endpoint difference, product or sum defines the reference value. Intermediate
floating overflow does not invalidate a mathematically representable result.

Apple Silicon CPU and x86-64 CPU accelerated versions obey the same whole-formula
strict reference formula, with the shared FP32-scaled final-result allowance. Their platform names remain separate operation keys.
Exact knot hits and clamped endpoints correctly convert the selected y directly
to output dtype, retaining its signed zero. No interpolation arithmetic or other
y values are required on that path.

For a true interpolation/extrapolation whose exact mathematical result is zero,
return -0 only when both selected endpoint y values are -0; otherwise return +0.
Nonzero exact results rounded to zero retain their mathematical sign. These rules
also preserve a constant -0 function under extrapolation, independent of signed
intermediate weights. No intermediate hardware zero sign defines the result.

## Confirmed finite-value policy

All mathematically used y and all query values must be finite. Every published output must
be finite as well; nonfinite input or destination overflow fails the affected
observation. Finite negative values and values greater than one are valid and
are not clipped. The x input retains its separately confirmed finite, strictly
increasing knot requirement. This follows the curve generator's finite-value
policy rather than NUM-04's IEEE-style successful NaN/Inf outputs.

## Confirmed requested-data support

For every nonempty request, one CPU Whole callback collects complete x, y and
query inputs, including recognized typed validation and upstream failures. It
validates all x knots and all query controls before y arithmetic, evaluates every query and every output column, and returns
one immutable dense output of shape [N] or [N,C]. Empty reads no payload; static
metadata validation still applies. Sparse demand restricts publication coverage,
but does not reduce input collection, computation or the complete output owner.

The mathematical stencil remains unchanged: an exact knot/clamp uses one y;
linear uses two endpoints; PCHIP uses its fixed local stencil. Generic y values
outside every evaluated stencil do not undergo an additional finite scan. Typed
validation and upstream execution cover complete inputs, including unused values.
All query rows and output columns are evaluated, so errors in unrequested rows
or columns can fail the run. Reject still performs full upstream collection.

Any input change invalidates the recorded output demand. Cache identity includes
complete input versions, profile, metadata and parameters. Numerical failures
have Run scope and publish no partial successful output; they do not provide
independent per-column Atom success. Input strides, offsets, zero strides and
negative strides remain legal. Packed output storage outlives the context.

## Exact mapping, validation and returned storage

The callback chooses an exact knot before interpolation. Otherwise interior
queries select the unique j with x[j]<query<x[j+1]; extrapolation uses j=0 or
K-2. Signed zeros compare numerically equal. Both linear endpoints undergo
finite validation even when equal. Whole execution, ownership and invalidation
follow the preceding section. All static edges are checked without payload.

## Reference algorithm and bounded resources

Search/classification costs O(K+N log K), followed by N scalar evaluations
(or N*C for multi). Classification is shared across columns. The complete dense
output costs b*N (or b*N*C) bytes; reserve it even for one requested cell. Input
collection and retained owners also require admission. The callback declares its
fixed exact arithmetic workspace, and the host-accounted knot vector has 8*K
element bytes plus allocator/metadata overhead. K<=65536 bounds its elements at
524288 bytes. No full slope table is required.

Poll work/cancellation during reads, binary search, exact arithmetic and before
publication. Capacity and work exhaustion return ResourceExhausted, with failed
output/workspace released. A giant logical broadcast can therefore fail a small
payload budget even for sparse demand. Resource limits do not authorize weaker
arithmetic. Backend, typed, upstream, stale and cancellation failures retain their
categories. Numeric failures are OperationFailed/InvalidDomain or final-output
ArithmeticOverflow, with Run scope and offending port/index where available.

## Error model

| Trigger | Phase and Status |
| --- | --- |
| Bad/missing dtype or domain-policy parameter | Compile/preflight; InvalidArgument / InvalidDomain |
| Wrong port type/rank/shape or unsupported K/N | Compile/preflight; TypeMismatch |
| Nonfinite x, repeated/decreasing knots, nonfinite demanded query/y | Evaluation; OperationFailed / InvalidDomain |
| Finite query outside domain under reject | Evaluation; OperationFailed / InvalidDomain |
| Final output conversion overflows to infinity | Evaluation; OperationFailed / ArithmeticOverflow |
| Insufficient exact arithmetic/index/output budget | Admission/evaluation; ResourceExhausted with existing resource reason |
| Wrong accelerated target, upstream failure, typed validation, cancellation or stale input | Preserve baseline capability/host Status and original identity |

Numeric/control failures have Run scope, identify the offending port/index,
and publish no partial successful Value. An upstream failure may precede the
callback's numerical rejection because input collection is complete.

## Acceptance and current verification

The public implementation fixture declares separate x/y/query bindings, creates
one selected versioned node with dtype and out_of_domain, names values, and
executes full and nonzero/disjoint requests via Compiler/ExecutionContext. It
must provide a real target/build/run command when implemented. Use the earlier
irregular-spacing fixture [3,1,3] and the three domain-policy outcomes, plus:

- Identity: x=y=[0,1,3], query=[2,0.5] -> [2,0.5].
- Extreme intermediates: x=[-MAX,MAX], y=[-MAX,MAX], query=[0] -> [+0],
  and x=[0,1], y=[-MAX,MAX], query=[0.5] -> [+0], for Float64 MAX.
- Float32 direct-rounding midpoint/neighbor cases from an independent exact
  rational oracle, both destination dtypes, subnormals and restored nondefault
  host rounding state. Strict compares exact result bits; accelerated compares
  final results against the shared FP32 bound. Exact selections and special
  values still compare bits.
- Exact knots and clamp read one y only; remote nonfinite y does not fail them.
  An interpolated/extrapolated query reads both endpoints. A bad remote x fails
  every requested observation retaining global topology; a bad unrequested query
  fails the run. Test full upstream collection even for reject.
- Constant -0, mixed signed-zero endpoints, zero cancellation and signed underflow;
  query repetitions/order, K=2 and size limits, dtype mixing and length errors.
- Read witnesses, query/x/y invalidation, source strides, warm/cache-off state,
  low work/capacity, cancellation, and owner lifetime after context teardown.

Numerical precision, exact dependency support and resource behavior are separate
acceptance obligations. The maintained public workflow and independent oracle
provide the current runtime acceptance evidence for the Proposed key.

## Existing implementation distinction

The existing curve.sample_linear takes controls[K,2] and static domain/count,
validates all controls and materializes Whole output. See the
[family source comparison](CRV-01_interpolate.md). It does not implement this
separate-x/y, explicit-query interface. The maintained target workflow and current validation boundary are recorded below.

## Specification dependencies

- [CRV-01 family decisions](CRV-01_interpolate.md).
- [Operator template](../../00-foundation/spec-template.md).
- [Foundation execution contract](../../00-foundation/contracts.md).

## Maintained implementation and validation

`plugins/ops/01-numeric/curve_interpolation.cpp` implements these three profile
keys. The public `photospider/numeric/curves.hpp` constructor is
`interpolate_linear_node`. Strict and Float64 outputs use exact rational evaluation and one final
destination rounding. Accelerated Float32 evaluation accepts only enclosures
whose endpoints round to the same Float32 result, preserving monotonicity;
unresolved cases use exact fallback. Exact cross products identify collinear
PCHIP stencils and reduce them to the linear formula. Complete input collection, Run failures and complete-output allocation follow
the Whole contract above; mathematical y stencils remain unchanged.

The [family implementation record](CRV-01_interpolate.md#maintained-implementation-and-validation)
contains the shared arithmetic/resource details and actual platform acceptance.
See [the editable workflow](../../../../examples/numeric_workflow/README.md#explicit-query-curves-crv-01)
for construction, explicit work budgets, commands and checked expected results.
The manual target is excluded from default builds and CTest/integration testing.
Specification status remains Proposed.
