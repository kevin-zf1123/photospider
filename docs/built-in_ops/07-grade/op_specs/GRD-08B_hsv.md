---
spec_schema_version: 1
id: GRD-08B
parent_id: GRD-08
function: hsv
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.hsv
---

# GRD-08B: Native HSV adjustment

Inherits the [family contract](GRD-08_hsl_hsv_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.hsv` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[3]` [delta_h,saturation_scale,value_scale] -> `values: T[S,C]`.

Select native HSV; S,V in [0,1]; scales>=0. Suggested [0,1,1]. Required hue_unit=radian|pi_multiple.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

When S>0, h_out=h+delta_h; otherwise copy h. S_out=clamp(S*saturation_scale,0,1); V_out=clamp(V*value_scale,0,1). Stored hue retains winding. Identity component controls copy.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

V=0.25 with scale=2 gives 0.5. Gray hue is unchanged.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
