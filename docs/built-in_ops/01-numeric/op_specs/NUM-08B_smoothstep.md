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

The strict profile defines the exact reference result below. Accelerated profiles follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md), including its range and fallback rules. Endpoint selections and special-value handling remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for registration and acceptance conventions. The operation has three dynamic Result inputs in order: `input`, `edge0`, and `edge1`. Each Result contains one tensor member, whose key may be any valid member key. All members must have identical `sample_shape()` and Float32 or Float64 dtype. `sample_shape()` includes batch axes; accepted rank is 1 through 8, every extent is positive, and the sample count is at most 2^40.

The output port is `values`, a Result with schema `photospider.tensor` and tensor member key `samples`. Its dtype and complete shape match the input members. Every output axis is an ordinary axis; output batch axes are empty. There are no static numeric parameters or implicit casts or broadcasts. Connect scalar edges through explicit broadcast operations. This operation uses the cubic smoothstep polynomial; it does not provide quintic smootherstep.

## Numeric contract

The caller supplies finite edges with `edge0 < edge1` at each computed coordinate. This is an unchecked numerical precondition. The kernel does not validate edges or promise a diagnostic for invalid samples.

With valid edges, a NaN input is quieted while preserving sign and payload. An input at or below `edge0` produces positive zero, including negative infinity. An input at or above `edge1` produces positive one, including positive infinity. For an interior finite input, define

$$
t=\frac{x-edge_0}{edge_1-edge_0},\qquad s=t^2(3-2t).
$$

The output is `RN_dtype(s)`, with one final destination rounding from the exact mathematical rational. Intermediate rounded `t` or polynomial operations do not define the result. Strict output is bitwise reproducible; accelerated floating output follows the shared FP32-scaled bound and remains in [0,1]. The exact midpoint maps to 0.5. Interior values rounding to zero produce positive zero. A positive exact edge width remains valid even when naive floating subtraction of the edges would overflow.

The implementation represents finite binary64 differences in a 104-limb (6656-bit) workspace. It forms `d = x - edge0` and `w = edge1 - edge0` as exact scaled integers, then evaluates the exact rational `d²(3w-2d)/w³` with fixed-width products and final rational rounding. For accelerated profiles, the ratio workspace may form outward bounds from the leading 53 bits. It accepts a candidate only when the interval is finite, avoids subnormal and overflow ranges, and the destination-rounded candidate is within a two-Float32-ULP absolute guard of both interval endpoints. If these checks cannot certify the result, the workspace falls back to exact integer quotient rounding. Float64 inputs are not narrowed to Float32. The 6656-bit capacity bound covers binary64 edge differences, the cubic numerator, quotient alignment, and the final significand. The Plan reserves the fixed workspace as Host scratch for each active worker lane. A resource admission failure returns `ResourceExhausted` before issue. Exact-arithmetic checkpoints query cancellation; they do not debit a per-sample work budget. See `plugins/ops/01-numeric/exact_interpolation.hpp` and `exact_ratio.hpp`.

## Fixed invocation execution and resources

Each requested output tile lowers to one CPU task. The task reads every input tile intersecting that output region, using each input's own batch, spatial and atomic-axis grid. It writes one separate preallocated dense output allocation in global coordinates. Empty selection emits no task and requires no input payload. Sparse requests compute only selected output tiles.

Prepare validates the exact read slots, output region, kernel contract and scratch capacity before binding invocation ABI 8. The prepared context is immutable and may be shared by independent Runs. Each active worker lane receives a separate fixed interpolation workspace reserved before issue. Kernels use nearest rounding with gradual underflow, restore the worker floating environment, and query cancellation per sample and during exact arithmetic. Failure discards unpublished output; accepted invocations retain input, output and scratch owners until return. Successful Result owners survive the execution context.

Static metadata validation checks dtype, complete sample shape, rank and the 2^40 sample bound. Numerical preconditions belong to the caller. Resource, upstream and cancellation failures retain their operational status codes. See [NUM-08 fixed interpolation execution](../interpolation-execution.md).

## Acceptance and implementation evidence

For a public execution case, use input `[-1,0,0.25,0.5,0.75,1,2]` with edges 0 and 1 and expect `[0,0,0.15625,0.5,0.84375,1,1]`; mix that output with endpoints 10 and 20 and expect `[10,10,11.5625,15,18.4375,20,20]`. Compare strict results with independent exact rational rounding. Include sparse and empty selections, nonzero origins, different batch/atomic input grids, extreme finite edges, infinity/NaN, fixed affine bindings, pre-cancellation and floating-environment restoration. Invalid numerical preconditions have no required error outcome.

The three profile keys implement fixed CPU tile invocations. `field.smoothstep` is a separate rank-2 operation with static Float64 edges and Float32 coverage output. Validate each available profile separately. The specification remains Proposed; implementation status does not establish cross-platform execution or performance.
