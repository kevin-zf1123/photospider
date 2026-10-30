---
spec_schema_version: 1
id: GRD-25D
parent_id: GRD-25
function: clahe_maps
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.clahe_maps
---

# GRD-25D: Generate local CLAHE maps

Inherits the [family contract](GRD-25_histogram_equalize_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.clahe_maps` performs the transformation below.

## Interface

`feature: T[H,W]`, `selection: UInt8[H,W]` or `Float64[H,W]` -> `maps: Float64[Ty,Tx,B]`, `identity: UInt8[Ty,Tx]`.

selection_mode defaults binary. cell_width,cell_height:Int64>=1 default 64; B:Int64 2..65536 default 256; clip_factor:Float64>=1 default 2; edge=reject|clamp defaults clamp. Tx=ceil(W/cell_width), Ty=ceil(H/cell_height).

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Cells start at the full-image origin, and edge cells are cropped. Build one histogram per cell using GRD-25A. Empty or single-positive-bin cells have identity=1 and map[i]=i/(B-1). Other cells use the clipping and redistribution equations in [GRD-25](GRD-25_histogram_equalize_contract.md), followed by map[i]=prefix[i]/N and identity=0.

## Behavior

A requested cell depends on its complete source rectangle. Histogram cells are algorithm parameters independent of output tiles. count, weights and boundaries use the cell actual extent.

## Acceptance

Uniform cells have identity=1. Check cropped edge cells and conserved histogram mass after clipping/redistribution.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
