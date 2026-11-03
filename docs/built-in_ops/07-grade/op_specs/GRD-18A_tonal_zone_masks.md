---
spec_schema_version: 1
id: GRD-18A
parent_id: GRD-18
function: tonal_zone_masks
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.tonal_zone_masks
---

# GRD-18A: Soft tonal-zone weights

Inherits the [family contract](GRD-18_tonal_zones_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.tonal_zone_masks` performs the transformation below.

## Interface

`feature: T[S,1]`, `edges: Float64[4]` [s0,s1,h0,h1] -> `zones: T[S,3]`.

s0<s1<=h0<h1. Suggested [0,0.25,0.75,1], expressed in the feature unit.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

The registered key is `grade.tonal_zone_masks`. The ordered required dynamic ports are `feature` and `edges`; the output port is `zones`.

| Parameter | Type | Meaning |
| --- | --- | --- |
| `axis` | Int64 | Required zero-based component axis within the tensor cell, excluding batch axes. The feature extent on this axis is one; the output extent is three. For spatial inputs, `layout.channel_axis` must be present and equal to `axis`. |
| `max_work` | Int64 | Optional per-invocation work limit; default 9223372036854775807, minimum zero. |

All other axes, batch axes and the source dtype are preserved. The edges tensor has no batch axes. The output component order is shadow, midtone, highlight; there are no alpha/AOV slots in this new feature tuple. No color conversion or feature extraction is implicit.

## Mathematics

Let smooth(t)=0 for t<=0, 1 for t>=1, otherwise t*t*(3-2*t). Set a=smooth((f-s0)/(s1-s0)), b=smooth((f-h0)/(h1-h0)). Output [1-a,a-b,b] in shadow,mid,highlight order. Consumers normalize the sum of stored weights.

### Current native algorithm/profile

Arithmetic is Float64, including for Float32 features, because the consumed edges are Float64. Each smoothstep returns exact zero or one at and beyond its edge endpoints. Inside an edge interval, evaluate `t=(f-lo)/(hi-lo)` and `(t*t)*(3-2*t)`; a finite normal reciprocal may replace division within the common native tolerance. An overflowing edge difference uses `(f/2-lo/2)/(hi/2-lo/2)`. Subnormal widths retain direct division. Form `[1-a,a-b,b]` in Float64 and convert each component once to T. Edges are dynamic invocation inputs. No payload-domain scan is performed.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

At f<=s0 weights are [1,0,0]; at s1<=f<=h0 they are [0,1,0]; at f>=h1 they are [0,0,1].

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
