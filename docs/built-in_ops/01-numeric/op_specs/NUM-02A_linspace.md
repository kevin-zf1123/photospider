---
spec_schema_version: 1
id: NUM-02A
parent_id: NUM-02
function: linspace
operation_family: numeric.linspace
proposed_operation_keys:
  - numeric.linspace_strict
  - numeric.linspace_fast
  - numeric.linspace_accelerated_apple_silicon
  - numeric.linspace_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
spec_revision: 0.2.0
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-02A: linspace

The strict key follows the exact reference below. Accelerated floating results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md); copies, endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md); the explicit rules here take precedence.

## Interface

`numeric.linspace_{strict,accelerated_apple_silicon,accelerated_x86_64}` creates a one-dimensional sequence from `start`, `end` and a static `count`. Each input is a Result containing exactly one tensor member at any member key, with complete `sample_shape()` `[1]`; both inputs are Float32 or Float64 and may differ in type.

| Parameter | Type and domain | Default |
| --- | --- | --- |
| `count` | Int64 in `[1, 1048576]` | Required |
| `dtype` | String `float32` or `float64` | Required in direct nodes; the helper defaults to Float64 |

The public SDK helper is `numeric::linspace_node`; it writes explicit `count` and `dtype` parameters, defaults to Float64 output and the strict CPU profile, and accepts another supported CPU profile. `numeric::SequenceInput` (built by `sequence_input` from a workflow input declaration) carries an input reference and an immutable single-tensor Result schema hint. Changing `count` or `dtype` requires recompilation. Malformed static parameters fail InvalidArgument; incompatible input schemas fail TypeMismatch.

The `values` output is a Result with schema `photospider.tensor`, member `samples`, shape `[count]` and the selected floating dtype. The `axis` output uses the same schema and member key, Float64 shape `[3]`, and `atomic_trailing_axes=1`, holding `[start, end, step]`. Both outputs use ordinary tensor axes and drop batch topology. Values and axis are independent output identities; requesting an axis component closes demand over the complete three-component tuple.

## Mathematics

For `N=count >= 2`, interpret the finite input endpoints as exact binary rational values and define each sample from its global index:

```text
values[0]   = RN_dtype(start)
values[N-1] = RN_dtype(end)
values[i]   = RN_dtype(((N-1-i)*exact(start) + i*exact(end))/(N-1)), 0<i<N-1
```

`RN_dtype` means direct round-to-nearest, ties-to-even in the selected output type. Float32 output never passes through an already-rounded interpolated Float64 value. Samples are computed from their index rather than by repeatedly adding a rounded step. For `N=1`, values is `[RN_dtype(start)]` and axis is `[start, start, +0]`.

For `N>=2`, axis is `[start, end, RN64((exact(end)-exact(start))/(N-1))]`, evaluated without an overflowing Float64 intermediate. Equal endpoints are valid and produce a positive-zero step. A nonzero exact step may round to signed zero. Axis keeps widened endpoint values even when values use Float32.

Equal endpoints and duplicate rounded samples are valid; consumers that require strictly ordered coordinates check that themselves. Endpoints preserve their signed-zero bits through conversion. An interior exact zero is negative only when both endpoints are negative zero; otherwise it is positive zero. A nonzero exact result rounded to zero keeps its sign. A constant negative-zero sequence contains negative-zero values, while its axis step is positive zero. Subnormals and gradual underflow to signed zero are valid. The caller's floating environment is preserved.

## Behavior

Finite active floating inputs and finite rounded selected floating outputs are unchecked caller preconditions; a selected axis requires a finite rounded step, and selected values tiles require finite rounded samples only within those tiles. Unselected outputs impose no output-range requirement.

For count greater than one, values and axis depend on both scalar inputs, even for endpoint-only output. Count one depends only on start; the second input is still schema-checked. Empty selections read nothing. Values use global indices; unselected coordinates are not computed. Selected outputs share one call: an operational failure prevents successful publication of that call's outputs. Unsupported accelerated targets return BackendUnavailable.

## Native fast numerical profile

The additional `numeric.linspace_fast` key is selected by `CpuNumericProfile::Fast`. For an interior position it computes binary64 `t=i/(count-1)` and `fma(t,end-start,start)`, or `(1-t)*start+t*end` if the subtraction overflows, then converts to the selected output dtype. Endpoints are selected directly, count=1 reads only start, and axis has `[start,end,(end-start)/(count-1)]` or `[start,start,0]`. This is ordinary native evaluation rather than NUM correctly rounded rational coordinates. Repeated addition is not used.

The endpoint-inclusive native grid and count-one/axis-only calculation are defined by the [CRV-04 fast profile](CRV-04_bake_lut1d.md#native-fast-numerical-profile).

## Acceptance

Independent Fraction/IEEE reference. Cases: values-only, axis-only, joint and empty selection; count-one with an unread end input; direct Float32 rounding; signed zero; exact extreme cancellation; sparse requests. `start=1`, `end=0x1.0000020000001p0`, `count=3`, Float32 linspace: the middle value is `0x3f800001`, avoiding Float64 intermediate double rounding.
