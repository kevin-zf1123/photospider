---
spec_schema_version: 1
id: GRD-21A
parent_id: GRD-21
function: saturation_linear
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.saturation_linear
---

# GRD-21A: Linear-RGB saturation

Inherits the [family contract](GRD-21_saturation_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.saturation_linear` performs the transformation below.

## Interface

`input: T[S,C]`, `scale: Float64[1]` -> `values: T[S,C]`.

Select linear RGB with explicit basis; scale>=0, suggested 1.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Y is the working-basis Y row applied to RGB. Return Y+scale*(RGB-Y). scale=1 copies. Equal-channel neutral inputs copy directly.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

scale=0 produces a neutral triple at Y. scale=1 is identity; neutral input stays neutral for all scales.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
