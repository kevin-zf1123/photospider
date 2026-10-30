---
spec_schema_version: 1
id: NUM-02B
parent_id: NUM-02
function: arange
operation_family: numeric.arange
proposed_operation_keys:
  - numeric.arange_strict
  - numeric.arange_accelerated_apple_silicon
  - numeric.arange_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
spec_revision: 0.2.0
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-02B: arange

The strict key follows the exact reference below. Accelerated floating results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md); copies, endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md); the explicit rules here take precedence.

## Interface

`numeric.arange_{strict,accelerated_apple_silicon,accelerated_x86_64}` creates a one-dimensional sequence from `start`, `step` and a static `count`. Each input is a Result containing exactly one tensor member at any member key, with complete `sample_shape()` `[1]`.

| Parameter | Type and domain | Default |
| --- | --- | --- |
| `count` | Int64 in `[1, 1048576]` | Required |
| `dtype` | String `int64`, `float32` or `float64` | Required in direct nodes; the helper defaults to Int64 when both input schemas are Int64, otherwise Float64 |

Integer mode requires both inputs to be Int64 and produces Int64 values and an Int64 axis. Floating mode accepts Float32 or Float64 inputs, including mixed floating types, and produces Float32 or Float64 values with a Float64 axis. Integer/float mixing requires an explicit cast.

The public SDK helper is `numeric::arange_node`; it emits explicit `count` and `dtype` and preserves the selected CPU profile key. `numeric::SequenceInput` (built by `sequence_input`) carries an input reference and an immutable single-tensor Result schema hint. Malformed static parameters fail InvalidArgument; incompatible schemas fail TypeMismatch.

The `values` output is a Result with schema `photospider.tensor`, member `samples`, shape `[count]` and the selected dtype. The `axis` output uses that schema and member key with shape `[3]` and `atomic_trailing_axes=1`, holding `[start, last, step]` (Int64 elements in integer mode, Float64 in floating mode). Both outputs use ordinary tensor axes and drop batch topology. A request for one axis component closes demand over the full tuple. Values and axis are independent output identities.

## Mathematics

For every global index `i`, define the exact value

```text
v_i = exact(start) + i * exact(step), 0 <= i < N
```

In integer mode this is evaluated exactly; final `v_i` representability is a caller precondition. An intermediate product outside Int64 is not an error if the final sum fits. In floating mode, input values are exact binary rationals and each `v_i` is rounded directly to the selected dtype using round-to-nearest, ties-to-even. A rounded step is not accumulated across samples, and Float32 output does not pass through an intermediate rounded Float64 sample.

For `N>=2`, integer axis is `[start, v_(N-1), step]`, with final Int64 representability required for the tuple. Floating axis is `[start, RN64(v_(N-1)), step]`; `last` is not narrowed to the Float32 value at the last sample. For `N=1`, integer values is the exact Int64 start; floating values is `[RN_dtype(start)]`; the axis is `[start, start, +0]`, without reading the step payload. Positive, negative and zero steps are valid. Rounded samples may repeat.

Finite floating inputs and finite rounded computed components are unchecked caller preconditions. Underflow to subnormals or signed zero is allowed. Index zero preserves the sign of start's zero through conversion. At later indices, an exact zero is negative only when start and step are both negative zero; otherwise it is positive. Nonzero exact values rounded to zero keep their sign. The singleton axis uses positive zero in its last component.

## Behavior

Finite active floating inputs, finite rounded selected floating outputs and representable selected Int64 outputs are unchecked caller preconditions. Unselected outputs impose no output-range requirement; a selected axis requires its complete tuple to satisfy the precondition.

For count greater than one, values and axis depend on both scalar inputs, even for endpoint-only output. Count one depends only on start; the second input is still schema-checked. Empty selections read nothing. Values use global indices; unselected coordinates are not computed. Selected outputs share one call: an operational failure prevents successful publication of that call's outputs. Unsupported accelerated targets return BackendUnavailable.

## Acceptance

Independent Fraction/IEEE reference. Cases: values-only, axis-only, joint and empty selection; count-one with an unread step input; direct Float32 rounding; signed zero; exact extreme cancellation; sparse requests. `INT64_MIN + 2*INT64_MAX = INT64_MAX-1`: the intermediate product exceeds Int64 while the final sample remains representable.
