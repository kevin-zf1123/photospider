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
clarification_status: complete
---

# CRV-05A: apply_lut1d

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) and the explicitly cited
[CRV-01A linear contract](CRV-01A_interpolate_linear.md) for unchanged rounding,
resources, host errors, ownership and acceptance. The uniform axis, arbitrary
input shape and singleton-table rules below take precedence. Clarification is
complete; specification status remains Proposed while the public operation is
implemented.

## Interface and static parameters

`apply_lut1d_node` maps every selected element of input through one scalar table. Required inputs in order are input, table and axis. Each is a Result with one tensor member under any schema id/version/key; shapes use complete `sample_shape()` including batch axes. Input has rank 1..8, positive extents and at most 2^40 elements. Table is `[L]`, with 1<=L<=1048576. Input and table independently accept Float32/Float64; axis is Float64 `[3]`. Static metadata checks apply to every edge, including schema-only edges.

Output `values` uses `photospider.tensor` v1/member `samples`, preserves the complete input shape as ordinary axes, has the selected Float32/Float64 dtype and carries no domain description. Source semantic facets, color roles and units are not propagated. Required String parameters are `dtype` (`float32` or `float64`) and `out_of_domain` (`reject`, `clamp` or `linear_extrapolate`). The constructor defaults policy to reject and output dtype from its authoring `input_type` hint. The compiler independently validates actual edges; direct nodes specify both parameters. Integer inputs require explicit conversion.

The operation uses fixed linear interpolation. At an exact knot it selects the table entry directly. Baking PCHIP values into a table produces a piecewise-linear LUT when consumed here. Per-channel tables belong to [CRV-05B](CRV-05B_apply_lut1d_channels.md); scalar-to-vector color ramps have a separate contract.

## Axis and domain preconditions

Let a=axis[0], b=axis[1], d=axis[2]. All three values must be finite. For L=1, b must preserve a's bit pattern and d must be +0. For L>=2 define:

```text
expected_step = RN64((exact(b)-exact(a))/(L-1))
x[0] = a; x[L-1] = b
x[j] = RN64(((L-1-j)*exact(a)+j*exact(b))/(L-1)), 0<j<L-1
```

Require a!=b, finite nonzero expected_step in direction b-a, and d bitwise equal to expected_step. The complete grid must be strictly increasing for ascending endpoints or strictly decreasing for descending endpoints. Signed-zero endpoints retain their bits; numerical duplicate knots violate the precondition. Coordinates use exact weighted endpoints followed by RN64, rather than accumulated step or a rounded fractional index.

These axis requirements are unchecked caller preconditions. Lookup reconstructs only the knots needed by bounded binary search and the selected stencil. Axis payload reads cover a and b for L>1; d has no separate payload dependency. Source tile rounding can still include d.

Consumed input queries must be finite. Under reject, each selected query must lie in the closed numerical endpoint domain; this is an unchecked precondition. Clamp selects the nearest numerical endpoint. Linear extrapolation uses that end's adjacent pair with its actual coordinate spacing. Array index zero may be the maximum-coordinate endpoint.

For L=1, reject requires selected input numerically equal to a; clamp and linear_extrapolate return the sole table entry for any finite input. All three policies execute the same direct table conversion. Input and axis retain static schema checks but have no payload dependencies, reads or numerical validation. Their producers can be omitted unless another output requires them. The axis and reject-equality preconditions still define valid calls.

## Interpolation and rounding

For a non-knot query q, select its adjacent bracketing pair j,j+1 in table order, or the nearest endpoint pair for extrapolation. The exact mathematical result is:

```text
((x[j+1]-q)*table[j] + (q-x[j])*table[j+1])/(x[j+1]-x[j])
```

The rounded Float64 knots define this function. Descending lookup swaps x and y together before evaluation with a positive denominator. Strict rounds the complete formula once to the output dtype. Accelerated profiles obey the [final FP32 contract](NUM_accelerated_contract.md), retain exact selection/conversion and use exact fallback when certification is inconclusive. Float64 evaluation retains the exact reference.

Knot/clamp/singleton selects one entry; interpolation/extrapolation selects two. Consumed table entries and selected outputs must be finite and outputs representable in the chosen dtype, as unchecked caller preconditions. Generic table values outside those stencils have no additional finite requirement. Exact selection preserves zero sign. Exact linear zero is -0 only when both selected entries are -0; other exact zeros are +0 and nonzero underflow keeps its mathematical sign. No approximation bound relative to the function used to generate the table is inferred.

## Physical demand, ownership and failures

The [family fixed-execution contract](CRV-05_apply_lut1d.md#fixed-execution-resources-and-caller-preconditions) defines task preparation, scratch, publication and failures. Each selected output tile reads its matching input region, all L scalar table rows, and axis endpoints for L>1, closed through each source grid. L=1 reads only table row zero and uses zero scratch. Only selected output tiles are computed and allocated. Input views support prepared offsets, signed/zero strides and unaligned storage.

A producer of the table must meet its own requirements for all rows in the declared closure, even if lookup mathematically selects only one pair. Expression and interpolation CRV-04 templates connect their values/axis outputs through ordinary graph edges. Count one can eliminate the consumer's axis edge; the producer's own selected values dependencies still apply. A sampler's equal or repeated coordinates are valid only when they also meet this consumer's axis preconditions.

Static preparation is reusable with compatible new bindings; each Run owns scratch and output storage. Selected tasks share their call's publication result, with output owners retained independently of the execution context. Numerical preconditions are unchecked; preparation, resources, cancellation and exact-arithmetic guards retain operational errors.

## Acceptance

For table=[0,0.25,1], axis=[0,1,0.5], input=[0,0.25,0.5,1], expect [0,0.125,0.25,1]. Reversing table with axis=[1,0,-0.5] gives the same function. A singleton table=[7], axis=[2,2,+0] maps input=2 to 7 under reject and any finite input to 7 under clamp/extrapolate.

Use independent exact coordinate reconstruction and rational interpolation for valid calls. Check both axis directions, mixed dtypes, exact knots, zero signs, representable narrowing boundaries, subnormals and all domain modes. Test dependency closure separately from arithmetic: sparse queries, complete scalar-table support, schema-only singleton inputs, ordinary versus atomic axis tiles, Empty, affine source views, maximum L, resource admission and cancellation. See the [family validation section](CRV-05_apply_lut1d.md#maintained-implementation-and-validation).

`field.apply_lut_1d` uses a rank-2 field, same-dtype table, static increasing domain and reject/clip policy through fixed basic tasks. `lut.apply_1d` accepts generic Float32 queries/table with static sample origin/step. Their interfaces and numerical rules are independent of this three-input dynamic-axis contract.

- [Family decisions](CRV-05_apply_lut1d.md).
- [Baking templates](CRV-04_bake_lut1d.md).
- [Operator template](../../00-foundation/spec-template.md).
