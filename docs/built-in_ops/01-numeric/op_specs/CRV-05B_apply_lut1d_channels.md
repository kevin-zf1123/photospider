---
spec_schema_version: 1
id: CRV-05B
parent_id: CRV-05
function: apply_lut1d_channels
operation_family: curve.apply_lut1d_channels
proposed_operation_keys:
  - curve.apply_lut1d_channels_strict
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

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Channel interface

Required inputs in order are input `[...,C]`, table `[L,C]` and shared Float64 axis `[3]`. Each Result has one tensor member under any schema id/version/key; complete `sample_shape()` includes batch axes. Input and table independently accept Float32/Float64. Require 1<=L<=1048576, C>=1, positive extents, input rank 1..8 and each logical product <=2^40. The final input axis equals the table's second extent. Rank-1 `[C]` is one vector and C=1 remains an explicit axis.

Output `values` uses `photospider.tensor` v1/member `samples`, preserves the complete input shape as ordinary axes, selects Float32/Float64 and carries no domain description. Static `dtype` and `out_of_domain` parameters, constructor defaults, axis formula, numerical preconditions and rounding follow [CRV-05A](CRV-05A_apply_lut1d.md). Metadata checks include the input/table C relation and every declared edge even when its payload is inactive. Each tensor has one dtype; the shared axis and policy apply to every channel.

Each input[...,c] uses table[:,c]. Queries and therefore knot/segment selection are independent per channel. This operation preserves C instead of mapping one scalar to a vector; no color conversion is inferred.

## Fixed demand and resources

Each selected output tile executes one fixed CPU task. For L>1 reads cover that input region, all L table rows in the output tile's final-axis interval and axis positions 0 and 1. Each source grid closes these regions independently; source atomic axes may cover extra channels or the step. Only the selected one/two entries of each channel participate in arithmetic. All required producer tiles must satisfy their producer's contract.

For L=1 only table row zero for selected channels has payload dependencies. Input and axis are schema-only and scratch is zero. Finite input, axis `[a,a,+0]` and reject's input equality to a remain unchecked caller preconditions, even though the constant-output kernel reads neither query nor axis. Unneeded producers may be absent from the selected closure.

The [family contract](CRV-05_apply_lut1d.md#fixed-execution-resources-and-caller-preconditions) defines constant-size per-invocation arithmetic scratch for L>1, selected output storage, prepared affine views, cancellation, fenv and ordered call publication. Lookup costs O(M log L) coordinate reconstructions for M selected elements. Sparse channels allocate only selected output tiles; physical table support scales with L and the selected column interval after source-grid closure. Output owners survive execution-context destruction. Invalid numerical preconditions carry no diagnostic guarantee.

## Acceptance

For table=[[0,10],[2,8]], axis=[0,1,1] and input=[[0,1],[0.25,0.5]], expect [[0,8],[0.5,9]]. This detects sharing a query/index across channels. Compare selected channels against independent scalar CRV-05A evaluation using exact rational coordinates and interpolation. Include both directions, every domain mode, mixed dtypes, signed zero, subnormals and singleton tables.

Check rank-1 `[C]`, C=1, complete sample shape with batch axes, ordinary/atomic source tile closure, independent query values, huge sparse C, reversed/unaligned/zero strides, concurrent Runs, compatible rebinding, Empty and malformed static/prepare descriptions. Inspect exact table-column reads and schema-only singleton input/axis edges. Connect the multi-function interpolation CRV-04 templates and verify source requirements over the selected table rows/columns. Exercise resource rejection, cancellation and retained owner lifetime. See the [family validation section](CRV-05_apply_lut1d.md#maintained-implementation-and-validation).

- [Family decisions](CRV-05_apply_lut1d.md).
- [Operator template](../../00-foundation/spec-template.md).
