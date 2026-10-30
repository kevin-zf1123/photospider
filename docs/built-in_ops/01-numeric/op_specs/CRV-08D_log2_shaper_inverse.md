---
spec_schema_version: 1
id: CRV-08D
parent_id: CRV-08
function: log2_shaper_inverse
proposed_operation_keys:
  - curve.log2_shaper_inverse_strict
  - curve.log2_shaper_inverse_accelerated_apple_silicon
  - curve.log2_shaper_inverse_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-08D: log2_shaper_inverse

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

## Interface

Ordered inputs are input, lower and upper Results, each with one tensor member under any schema id/version/key. Complete `sample_shape()` includes batch axes. Input is Float32/Float64, rank 1..8, positive extents and <=2^40 elements. Bounds are same-dtype [1] scalars. Output `values` uses `photospider.tensor` v1/member `samples`, preserving complete input shape/dtype as ordinary axes with no domain description. There are no static mode, dtype or clipping parameters. The authoring helper is `log2_shaper_inverse_node`. Shape/dtype mismatch is TypeMismatch, and unsupported profiles fail BackendUnavailable.

## Mathematics

The exact mathematical formula is lower*(upper/lower)^input, rounded as one expression. Bounds must be finite and satisfy 0<lower<upper as an unchecked caller precondition (no diagnostic or error precedence is guaranteed). Input IEEE special values remain part of the valid numerical interface; overflow/underflow are successful IEEE results.

Exact endpoints, NaN quieting/payload/sign, infinity and zero extensions, monotonicity and profile precision follow the [family contract](CRV-08_shaper.md#mathematics). t=+0/-0 selects lower exactly, t=1 selects upper exactly, -Inf maps to +0 and +Inf maps to +Inf. These transformations do not change a color description or perform tone mapping.

## Behavior

Each output element depends on its input element and both scalar bounds. Region and tile results equal the corresponding slice of the whole result.

## Acceptance

L=1,U=16, input=[-0.25,0,0.25,0.5,1,1.25] gives [0.5,1,2,4,16,32]. Expected values come from an exact whole-formula evaluation, including IEEE payloads, exact landmarks, extremes, subnormal results and monotonic groups. Rounded round trips alone are insufficient. See [family acceptance](CRV-08_shaper.md#acceptance).
