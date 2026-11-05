---
spec_schema_version: 1
id: GRD-26B
parent_id: GRD-26
function: vignette
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.vignette
---

# GRD-26B: Apply an elliptical vignette

Inherits the [family contract](GRD-26_vignette_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.vignette` performs the transformation below.

## Interface

`input: T[H,W,C]`, `coordinates: Float64[H,W,2]` -> `values: T[H,W,C]`.

Linear RGB/Gray; parameters are those of GRD-26A. For pixel units the supplied coordinates are the global pixel centers (x+0.5,y+0.5).

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Calculate the Float64 EV field by GRD-26A, then apply the exposure-field equation GRD-01B. Preserve the source geometry and bypass channels.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

center_ev=edge_ev=0 gives identity. A -1 EV outer region halves its source values.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
