---
spec_schema_version: 1
id: GRD-26A
parent_id: GRD-26
function: vignette_ev
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.vignette_ev
---

# GRD-26A: Generate an elliptical vignette EV field

Inherits the [family contract](GRD-26_vignette_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.vignette_ev` performs the transformation below.

## Interface

`coordinates: Float64[H,W,2]` in [x,y] order -> `ev: Float64[H,W,1]`.

Required static center:Float64[2], radii:Float64[2]>0. rotation_pi:Float64=0; inner:Float64=0; outer:Float64=1 with 0<=inner<outer; center_ev:Float64=0; edge_ev:Float64=-1. Coordinates and radii share declared units.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

With theta=pi*rotation_pi, dx=x-cx,dy=y-cy, u=(cos(theta)*dx+sin(theta)*dy)/rx, v=(-sin(theta)*dx+cos(theta)*dy)/ry, r=hypot(u,v). t=clamp((r-inner)/(outer-inner),0,1); ev=center_ev+(edge_ev-center_ev)*t*t*(3-2*t). Equal EV endpoints select that value.

## Behavior

E support on the coordinate tuple. Coordinates are absolute in the declared domain and remain independent of tile origin.

## Acceptance

At the center output center_ev. At or beyond outer output edge_ev. Positive edge EV produces edge brightening.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
