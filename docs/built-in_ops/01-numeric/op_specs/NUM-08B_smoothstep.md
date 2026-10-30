---
spec_schema_version: 1
id: NUM-08B
parent_id: NUM-08
function: smoothstep
proposed_operation_keys:
  - numeric.smoothstep_strict
  - numeric.smoothstep_accelerated_apple_silicon
  - numeric.smoothstep_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-08B: smoothstep

The strict profile defines the exact reference result below. Accelerated profiles follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Endpoint selections and special-value handling are exact. Inherits the [NUM baseline](NUM_common_contract.md).

## Interface

Three dynamic Result inputs in order: `input`, `edge0` and `edge1`. Each Result contains one tensor member, whose key may be any valid member key. All members have identical `sample_shape()` (including batch axes) and Float32 or Float64 dtype. Rank is 1..8, every extent is positive, and the sample count is at most 2^40.

The output port is `values`, a Result with schema `photospider.tensor` and tensor member key `samples`. Its dtype and complete shape match the input members. Every output axis is an ordinary axis; output batch axes are empty. There are no static parameters or implicit casts or broadcasts; scalar edges are connected through explicit broadcast operations. This operation uses the cubic smoothstep polynomial; it does not provide quintic smootherstep. `field.smoothstep` is a separate rank-2 operation.

## Mathematics

The caller supplies finite edges with `edge0 < edge1` at each computed coordinate (unchecked numerical precondition).

With valid edges, a NaN input is quieted with sign and payload preserved. An input at or below `edge0` produces positive zero, including negative infinity. An input at or above `edge1` produces positive one, including positive infinity. For an interior finite input,

$$
t=\frac{x-edge_0}{edge_1-edge_0},\qquad s=t^2(3-2t).
$$

The output is `RN_dtype(s)`, with one final destination rounding from the exact mathematical rational; intermediate rounded `t` or polynomial operations do not define the result. Strict output is bitwise reproducible; accelerated floating output follows the shared FP32-scaled bound and stays in [0,1]. The exact midpoint maps to 0.5. Interior values rounding to zero produce positive zero. A positive exact edge width remains valid even when naive floating subtraction of the edges would overflow. Float64 inputs are not narrowed to Float32.

## Behavior

Each output sample depends on the input and both edges at the same coordinate.

## Acceptance

Input `[-1,0,0.25,0.5,0.75,1,2]` with edges 0 and 1 gives `[0,0,0.15625,0.5,0.84375,1,1]`; mixing that output with endpoints 10 and 20 gives `[10,10,11.5625,15,18.4375,20,20]`. Compare strict results with independent exact rational rounding. Cases: extreme finite edges, infinity, NaN.
