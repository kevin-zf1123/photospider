---
spec_schema_version: 1
id: GRD-18A
parent_id: GRD-18
function: tonal_zone_masks
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
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

## Mathematics

Let smooth(t)=0 for t<=0, 1 for t>=1, otherwise t*t*(3-2*t). Set a=smooth((f-s0)/(s1-s0)), b=smooth((f-h0)/(h1-h0)). Output [1-a,a-b,b] in shadow,mid,highlight order. Consumers normalize the sum of stored weights.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

At f<=s0 weights are [1,0,0]; at s1<=f<=h0 they are [0,1,0]; at f>=h1 they are [0,0,1].

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
