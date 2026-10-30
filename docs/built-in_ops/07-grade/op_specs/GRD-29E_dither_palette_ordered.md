---
spec_schema_version: 1
id: GRD-29E
parent_id: GRD-29
function: dither_palette_ordered
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.dither_palette_ordered
---

# GRD-29E: Ordered palette dither

Inherits the [family contract](GRD-29_dither_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.dither_palette_ordered` performs the transformation below.

## Interface

`input: T[H,W,C]`, `palette: T[K,3]`, `thresholds: Float64[My,Mx]` -> `values: T[H,W,C]`.

Select relative linear RGB. Palette entries share the source RGB basis and dtype; 1<=K<=256. 1<=Mx,My<=4096; thresholds in (0,1). origin:Int64[2]=[0,0].

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Select the nearest projected palette segment and its fraction f as defined by the family. At the global coordinate read the repeating threshold as GRD-29D without channel phase; choose the second endpoint iff threshold<f. A selected entry is copied from the supplied palette, so every adjusted RGB tuple belongs to that palette.

## Behavior

E on source/threshold or noise, plus the full palette. Absolute coordinate phase is preserved. Alpha/AOV slots bypass.

## Acceptance

A one-color palette produces that color everywhere. Every output RGB is bitwise one supplied palette entry; check palette ties, duplicate colors and tile consistency.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
