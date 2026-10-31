---
spec_schema_version: 1
id: CRV-05B
parent_id: CRV-05
function: apply_lut1d_channels
operation_family: curve.apply_lut1d_channels
proposed_operation_keys:
  - curve.apply_lut1d_channels_strict
  - curve.apply_lut1d_channels_fast
  - curve.apply_lut1d_channels_accelerated_apple_silicon
  - curve.apply_lut1d_channels_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-05B: apply_lut1d_channels

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

## Interface

Required inputs in order are input `[...,C]`, table `[L,C]` and shared Float64 axis `[3]`. Each Result has one tensor member under any schema id/version/key; complete `sample_shape()` includes batch axes. Input and table independently accept Float32/Float64. Require 1<=L<=1048576, C>=1, positive extents, input rank 1..8 and each logical product <=2^40. The final input axis equals the table's second extent. Rank-1 `[C]` is one vector and C=1 remains an explicit axis.

Output `values` uses `photospider.tensor` v1/member `samples`, preserves the complete input shape as ordinary axes, selects Float32/Float64 and carries no domain description. Static `dtype` and `out_of_domain` parameters, constructor defaults, axis formula, numerical preconditions and rounding follow [CRV-05A](CRV-05A_apply_lut1d.md). Metadata checks include the input/table C relation and every declared edge even when its payload is inactive. Each tensor has one dtype; the shared axis and policy apply to every channel.

## Mathematics

Each input[...,c] uses table[:,c]. Queries and therefore knot/segment selection are independent per channel. This operation preserves C instead of mapping one scalar to a vector; no color conversion is inferred.

## Behavior

Each output element depends on its input element, the table column of its channel (all L rows; only the selected one or two entries participate in arithmetic) and, for L>1, axis positions 0 and 1. For L=1 only table row zero of the channel is a dependency, input and axis have no payload dependency, and finite input, axis `[a,a,+0]` and reject's input equality to a remain unchecked caller preconditions. Region and tile results equal the corresponding slice of the whole result. Invalid numerical preconditions carry no diagnostic guarantee.

## Native fast numerical profile

The additional `_fast` key is selected by `CpuNumericProfile::Fast`. Its native arithmetic algorithm is defined in the [family profile](CRV-05_apply_lut1d.md#native-fast-numerical-profile); it replaces the strict/accelerated rounding guarantees for this key while retaining the stated parameter and shape interface.

## Acceptance

For table=[[0,10],[2,8]], axis=[0,1,1] and input=[[0,1],[0.25,0.5]], expect [[0,8],[0.5,9]]. This detects sharing a query/index across channels. Selected channels match independent scalar CRV-05A evaluation using exact rational coordinates and interpolation. Include both directions, every domain mode, mixed dtypes, signed zero, subnormals and singleton tables, rank-1 `[C]` and C=1. Connect the multi-function interpolation CRV-04 templates and verify source requirements over the selected table rows/columns.

## References

- [Family decisions](CRV-05_apply_lut1d.md).
- [Operator template](../../00-foundation/spec-template.md).
