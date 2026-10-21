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
clarification_status: complete
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
repeated query. Global knot ordering is a caller precondition.

## Confirmed dtype contract

Each of x, y and query independently accepts Float32 or Float64. They may have
different floating dtypes. Input values denote their exact binary numerical
values; Float32 widening to Float64 is exact. No input integer dtype or implicit
integer conversion is provided; connect an explicit cast where needed.

Output values selects Float32 or Float64 through a static dtype parameter,
defaulting to Float64. Conversion/rounding boundaries and special-value handling
are specified separately rather than inferred from native mixed-type arithmetic.

Each input Result contains one tensor member under any schema id and member key; its `sample_shape()` supplies x[K], y[K] and query[N]. Accept legal immutable zero/negative strides, nonzero offsets and unaligned storage. Output port `values` is a Result using schema `photospider.tensor`, member `samples`, shape [N] (or [N,C] for the multi-function forms), and no attached domain description. Batch dimensions are ordinary sample axes; no axis output or implicit physical/color units are created.

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

- reject requires the caller to keep consumed queries within the closed knot interval.
- clamp returns the nearest endpoint's y value with the selected output conversion.
- linear_extrapolate extends the first or last segment's straight line using
  that segment's endpoints in the same linear expression.

For x=[0,1], y=[0,2], query=[2], `reject` is outside its caller contract; clamp gives [2] and linear_extrapolate gives [4].
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

Finite x and mathematically used y/query values, strict x ordering and finite representable destination values are caller preconditions. Finite negative values and values greater than one remain valid and are not clipped. These numerical preconditions are not runtime validation passes or error-reporting guarantees.

## Confirmed requested-data support

Execution, physical read closure, caller preconditions, resource admission and ownership follow the [family fixed execution contract](CRV-01_interpolate.md#fixed-execution-resources-and-caller-preconditions). Each selected output tile reads all x, all y rows for its selected columns, and its query row range through the corresponding input grids. Only the tile's global output coordinates are evaluated. Mathematical stencil selection remains defined by this member specification.

## Exact mapping, validation and returned storage

The kernel chooses an exact knot before interpolation. Otherwise an interior query selects the unique j with x[j]<query<x[j+1]; extrapolation uses j=0 or K-2. Signed zeros compare numerically equal. Numerical evaluation reads the selected y stencil from the declared fixed views. The family contract defines physical input tile support and independent output ownership.

## Reference algorithm and bounded resources

The [family resource contract](CRV-01_interpolate.md#fixed-execution-resources-and-caller-preconditions) applies. Each task has one preallocated `ExactCurve` arena and selected output storage. Direct binary search replaces a copied knot array; classification is shared by columns within the task. Resources scale with declared input tiles, selected outputs and granted-lane scratch, rather than the complete logical output. Capacity checks and cancellation preserve their operational categories.

## Error model

Static parameter errors return `InvalidArgument`; incompatible port metadata returns `TypeMismatch`. Unsupported profiles, resource admission, cancellation, stale sources and required upstream task failures retain their operational categories. Finite-value, ordering, reject-domain and output-representability requirements are unchecked caller preconditions. Internal exact-arithmetic capacity and zero-denominator checks protect the bounded arithmetic implementation. Failed calls publish no partial successful Result.

## Acceptance requirements

Acceptance requirements: the public implementation fixture declares separate x/y/query bindings, creates one selected versioned node with dtype and out_of_domain, names values, and executes full and nonzero/disjoint requests via Compiler/ExecutionContext. Use the earlier
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
  An interpolated/extrapolated query uses both endpoints. Verify that Plan reads contain all x, selected y columns and the selected query range, including source-grid tile rounding.
- Constant -0, mixed signed-zero endpoints, zero cancellation and signed underflow;
  query repetitions/order, K=2 and size limits, dtype mixing and length errors.
- Read witnesses, query/x/y invalidation, source strides, concurrent Runs and preparation reuse,
  low work/capacity, cancellation, and owner lifetime after context teardown.

Numerical precision, exact dependency support and resource behavior are separate
acceptance obligations. These cases remain acceptance requirements.

## Actual focused validation

For temporary public-path acceptance, compare all four interpolation forms with independent small-input references. Cover exact linear and nonlinear PCHIP results, admitted profiles, Float32/Float64, knot/clamp signed zero, fixed read signatures, sparse query/column selection, limits, cancellation and output lifetime. Include `linspace -> bake_lut1d_pchip` and `resample_linear` compositions when changing the corresponding helpers.

Create and run these checks for the current operation change, then remove the
temporary checker and its build registration.

## Existing implementation distinction

The existing curve.sample_linear takes controls[K,2] and static domain/count,
uses one fixed task for selected output tiles and computes global grid coordinates. See the
[family source comparison](CRV-01_interpolate.md). It does not implement this
separate-x/y, explicit-query interface. The maintained target workflow and current validation boundary are recorded below.

## Specification dependencies

- [CRV-01 family decisions](CRV-01_interpolate.md).
- [Operator template](../../00-foundation/spec-template.md).
- [Foundation execution contract](../../00-foundation/contracts.md).

## Maintained implementation and validation

`plugins/ops/01-numeric/curve_interpolation.cpp` implements these three profile
keys. The public `photospider/ops/numeric/curves.hpp` constructor is
`interpolate_linear_node`. Strict and Float64 outputs use exact rational evaluation and one final
destination rounding. Accelerated Float32 evaluation accepts only enclosures
whose endpoints round to the same Float32 result, preserving monotonicity;
unresolved cases use exact fallback. Exact cross products identify collinear
PCHIP stencils and reduce them to the linear formula. Fixed task reads, selected output allocation and caller preconditions follow the family execution contract; mathematical y stencils remain unchanged.

The [family implementation record](CRV-01_interpolate.md#maintained-implementation-and-validation)
contains the shared arithmetic/resource details and current validation boundary.
This
operation's specification status remains Proposed.
