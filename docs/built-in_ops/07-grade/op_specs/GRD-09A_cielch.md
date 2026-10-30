---
spec_schema_version: 1
id: GRD-09A
parent_id: GRD-09
function: cielch
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.cielch
---

# GRD-09A: CIELCh adjustment

Inherits the [family contract](GRD-09_lch_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.cielch` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[3]` [delta_l,chroma_scale,delta_h] -> `values: T[S,C]`.

Select CIELCh; l=L*/100 and native CIE C*; C>=0, chroma_scale>=0. Suggested [0,1,0]. Required hue_unit=radian|pi_multiple.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

L_out=L+delta_l; C_out=C*chroma_scale. If C>0, h_out=h+delta_h; otherwise copy h. Lightness is unclipped and hue retains winding. Identity component controls copy.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Chroma scale zero makes C zero. A lightness increment of 0.1 changes only the first native coordinate.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
