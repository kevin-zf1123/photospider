---
spec_schema_version: 1
id: GRD-01B
parent_id: GRD-01
function: exposure_field
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.exposure_field
---

# GRD-01B: Exposure from a spatial EV field

Inherits the [family contract](GRD-01_exposure_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.exposure_field` performs the transformation below.

## Interface

`input: T[S,C]`, `ev: Float64[S,1]` -> `values: T[S,C]`.

Linear RGB or linear-Y Gray. The field uses stops and exactly matches S; its value is shared over the selected color components.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Apply [GRD-01A](GRD-01A_exposure.md) with ev[p] at each coordinate p. A zero field sample copies the corresponding source pixel.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Input [0.25,0.25] and EV [0,1] gives [0.25,0.5]. A crop uses the matching full-image EV coordinates.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
