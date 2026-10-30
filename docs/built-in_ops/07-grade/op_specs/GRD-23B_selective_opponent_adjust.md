---
spec_schema_version: 1
id: GRD-23B
parent_id: GRD-23
function: selective_opponent_adjust
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.selective_opponent_adjust
---

# GRD-23B: Adjust selected opponent colors

Inherits the [family contract](GRD-23_selective_color_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.selective_opponent_adjust` performs the transformation below.

## Interface

`input: T[S,C]`, `weights: Float64[S,K]`, `deltas: Float64[K,3]` [dL,da,db] -> `values: T[S,C]`.

Select CIELAB or OKLab. 1<=K<=256; weights in [0,1]. overlap=sum|normalize, default normalize.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

D_c=sum_j(w_j*delta[j,c]); d=1 for sum and max(1,sum_j w_j) for normalize. Return x_c+D_c/d. Zero computed D_c copies its source component. All selection weights refer to original source coordinates.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Two unit masks with identical delta give twice the delta in sum mode and one delta in normalize mode. All-zero masks copy.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
