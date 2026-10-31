---
spec_schema_version: 1
id: GRD-04B
parent_id: GRD-04
function: levels_signed
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.levels_signed
---

# GRD-04B: Signed levels

Inherits the [family contract](GRD-04_levels_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.levels_signed` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[Csel,5]` with rows [black,white,gamma,out0,out1] -> `values: T[S,C]`.

The domains and suggested controls are those of [GRD-04A](GRD-04A_levels_bounded.md).

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Current parameters

The operation uses the [current family selector and native computation profiles](GRD-04_levels_contract.md#current-parameters), including ordered `components:String` and required dynamic `Float64[Csel,5]` controls.

## Mathematics

`u=(x-black)/(white-black); y=out0+(out1-out0)*spow(u,1/gamma)`. Apply the signed power from GRD-02B. Default controls copy all finite source values; u=0/1 selects out0/out1.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Default controls preserve [-1,0.25,2]. gamma=2 maps u=-0.25 to -0.5 when out0=0,out1=1.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
