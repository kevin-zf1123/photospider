---
spec_schema_version: 1
id: GRD-29G
parent_id: GRD-29
function: dither_palette_floyd_steinberg
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.dither_palette_floyd_steinberg
---

# GRD-29G: Floyd-Steinberg palette dither

Inherits the [family contract](GRD-29_dither_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.dither_palette_floyd_steinberg` performs the transformation below.

## Interface

`input: T[H,W,C]`, `palette: T[K,3]` -> `values: T[H,W,C]`.

Select relative linear RGB. Palette entries share the source RGB basis and dtype; 1<=K<=256. scan=raster|serpentine defaults serpentine.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Convert source RGB and palette colors to OKLab. Visit the family Float64 scan; add accumulated OKLab error, choose the nearest palette OKLab tuple by squared Euclidean distance and smallest palette index on a tie, and diffuse the difference from that palette tuple. Publish the original RGB palette entry. A selected entry is copied from the supplied palette, so every adjusted RGB tuple belongs to that palette.

## Behavior

S support on canonical predecessors and the full palette; a full source-image support superset is permitted. Scan error never resets at tile boundaries. Alpha/AOV slots bypass.

## Acceptance

A one-color palette produces that color everywhere. Every output RGB is bitwise one supplied palette entry; check palette ties, duplicate colors and tile consistency.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
