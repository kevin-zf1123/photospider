---
spec_schema_version: 1
id: GRD-04A
parent_id: GRD-04
function: levels_bounded
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.levels_bounded
---

# GRD-04A: Bounded levels

Inherits the [family contract](GRD-04_levels_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.levels_bounded` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[Csel,5]` with rows [black,white,gamma,out0,out1] -> `values: T[S,C]`.

black<white; gamma>0. Suggested [0,1,1,0,1]. Output endpoints may be reversed.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Current parameters

The operation uses the [current family selector and native computation profiles](GRD-04_levels_contract.md#current-parameters), including ordered `components:String` and required dynamic `Float64[Csel,5]` controls.

## Mathematics

`u=(x-black)/(white-black); v=clamp(u,0,1); y=out0+(out1-out0)*pow(v,1/gamma)`. v=0/1 selects the corresponding endpoint. Default controls copy in-range input and clamp extended input.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Default controls map [-1,0.25,2] to [0,0.25,1]. gamma=2 maps u=0.25 to 0.5.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
