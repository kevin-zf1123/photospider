---
spec_schema_version: 1
id: GRD-29F
parent_id: GRD-29
function: dither_palette_noise
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.dither_palette_noise
---

# GRD-29F: Palette dither with explicit noise

Inherits the [family contract](GRD-29_dither_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.dither_palette_noise` performs the transformation below.

## Interface

`input: T[H,W,C]`, `palette: T[K,3]`, `noise: Float32/64[H,W,1]` -> `values: T[H,W,C]`.

Select relative linear RGB. Palette entries share the source RGB basis and dtype; 1<=K<=256. Noise is in [0,1).

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Select the nearest projected palette segment and its fraction f as defined by the family. Choose the second endpoint iff noise<f. A selected entry is copied from the supplied palette, so every adjusted RGB tuple belongs to that palette.

## Behavior

E on source/threshold or noise, plus the full palette. Absolute coordinate phase is preserved. Alpha/AOV slots bypass.

## Acceptance

A one-color palette produces that color everywhere. Every output RGB is bitwise one supplied palette entry; check palette ties, duplicate colors and tile consistency.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
