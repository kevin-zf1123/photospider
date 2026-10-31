---
spec_schema_version: 1
id: CRV-01A
parent_id: CRV-01
function: interpolate_linear
operation_family: curve.interpolate_linear
proposed_operation_keys:
  - curve.interpolate_linear_strict
  - curve.interpolate_linear_fast
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

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for registration, descriptor inference, platform availability, floating environment, error attribution and acceptance, and the [CRV-01 family contract](CRV-01_interpolate.md) for ports, schemas and preconditions. The explicit finite-value, input-layout and query-dependent rules below take precedence.

## Interface

Evaluate a single piecewise-linear function through supplied known points at explicit dynamic query positions. Inputs in order are x[K], y[K], query[N]; output values[N] evaluates the function at each query position. The mathematical curve passes through the supplied (x,y) points. Input descriptors determine K and N; query values are runtime data rather than a static interval parameter.

This operation has no additional C axis (multiple functions are CRV-01C). Linear and PCHIP are independently named operations; this operation has no method switch. Operation keys are `curve.interpolate_linear_strict`, `curve.interpolate_linear_accelerated_apple_silicon` and `curve.interpolate_linear_accelerated_x86_64`; the public constructor is `interpolate_linear_node`.

Require 2<=K<=65536, 1<=N<=2^40. Each of x, y and query independently accepts Float32 or Float64, and they may differ. Input values denote their exact binary numerical values; Float32 widening to Float64 is exact. No integer dtype or implicit integer conversion is provided; connect an explicit cast where needed.

Each input Result contains one tensor member under any schema id and member key; its `sample_shape()` supplies x[K], y[K] and query[N]. Output port `values` is a Result using schema `photospider.tensor`, member `samples`, shape [N], and no attached domain description. Batch dimensions are ordinary sample axes; no axis output or implicit physical/color units are created.

| Static parameter | Type and domain | Constructor default |
| --- | --- | --- |
| dtype | String float32 or float64 | float64 |
| out_of_domain | String reject, clamp or linear_extrapolate | reject |

Direct WorkflowDocument nodes supply both parameters explicitly. K/N derive from descriptors rather than count parameters. Missing/extra ports, unsupported dtype, wrong rank, x/y length mismatch and size excess fail compile/preflight.

## Mathematics

For a selected segment with distinct endpoint abscissas x[j], x[j+1], the linear expression is

```text
t = (query[i] - x[j]) / (x[j+1] - x[j])
values[i] = (1-t)*y[j] + t*y[j+1]
```

An exact knot is chosen before interpolation. Otherwise an interior query selects the unique j with x[j]<query<x[j+1]; extrapolation uses j=0 or K-2. Signed zeros compare numerically equal.

Domain policy, for a query below the first or above the last knot:

- reject requires the caller to keep consumed queries within the closed knot interval.
- clamp returns the nearest endpoint's y value with the selected output conversion.
- linear_extrapolate extends the first or last segment's straight line using that segment's endpoints in the same linear expression.

For x=[0,1], y=[0,2], query=[2], `reject` is outside its caller contract; clamp gives [2] and linear_extrapolate gives [4]. The finite endpoint interval includes both endpoints. No automatic clipping of the y result or implicit change of the query position is introduced.

Strict evaluates the entire mathematical linear interpolation/extrapolation expression, treating input floats as exact binary rationals, and rounds only the final result directly to the output dtype using nearest/ties-to-even. No intermediate rounded t, endpoint difference, product or sum defines the reference value. Intermediate floating overflow does not invalidate a mathematically representable result.

Apple Silicon CPU and x86-64 CPU accelerated versions follow the same whole-formula reference with the shared FP32-scaled final-result allowance. Exact knot hits and clamped endpoints correctly convert the selected y directly to output dtype, retaining its signed zero.

For a true interpolation/extrapolation whose exact mathematical result is zero, return -0 only when both selected endpoint y values are -0; otherwise return +0. Nonzero exact results rounded to zero retain their mathematical sign. These rules also preserve a constant -0 function under extrapolation, independent of signed intermediate weights.

## Behavior

Query positions may be unsorted and may repeat; their original order is preserved in values. No duplicate query rejection or automatic resampling is introduced. Exact knots and clamp depend on one y only; remote nonfinite y does not affect them. An interpolated/extrapolated query depends on both segment endpoints. Region and tile results equal the corresponding slice of the whole result.

Finite x and mathematically used y/query values, strict x ordering (x[j]<x[j+1]; no sorting, duplicate merging or decreasing x) and finite representable destination values are caller preconditions. Finite negative values and values greater than one remain valid and are not clipped. These are not runtime validation passes or error-reporting guarantees.

Static parameter errors return `InvalidArgument`; incompatible port metadata returns `TypeMismatch`. Operational failures retain their categories, and failed calls publish no partial successful Result.

## Native fast numerical profile

The additional `_fast` key is selected by `CpuNumericProfile::Fast`. Its native arithmetic algorithm is defined in the [family profile](CRV-01_interpolate.md#native-fast-numerical-profile); it replaces the strict/accelerated rounding guarantees for this key while retaining the stated parameter and shape interface.

## Acceptance

Analytic fixture: x=[0,1,3], y=[0,2,4], query=[2,0.5,2] gives values=[3,1,3] (irregular knot spacing, out-of-order queries and a repeated query), plus the three domain-policy outcomes above, and:

- Identity: x=y=[0,1,3], query=[2,0.5] -> [2,0.5].
- Extreme intermediates: x=[-MAX,MAX], y=[-MAX,MAX], query=[0] -> [+0], and x=[0,1], y=[-MAX,MAX], query=[0.5] -> [+0], for Float64 MAX.
- Float32 direct-rounding midpoint/neighbor cases from exact rational evaluation, both destination dtypes, subnormals and restored nondefault host rounding state. Strict compares exact result bits; accelerated compares final results against the shared FP32 bound. Exact selections and special values still compare bits.
- Exact knots and clamp read one y only; remote nonfinite y does not fail them.
- Constant -0, mixed signed-zero endpoints, zero cancellation and signed underflow; query repetitions/order, K=2 and size limits, dtype mixing and length errors.

## References

- [CRV-01 family decisions](CRV-01_interpolate.md).
- [Operator template](../../00-foundation/spec-template.md).
- [Foundation execution contract](../../00-foundation/contracts.md).
