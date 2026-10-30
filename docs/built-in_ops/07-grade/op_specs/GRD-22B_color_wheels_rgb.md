---
spec_schema_version: 1
id: GRD-22B
parent_id: GRD-22
function: color_wheels_rgb
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.color_wheels_rgb
---

# GRD-22B: RGB tonal wheels

Inherits the [family contract](GRD-22_split_tone_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.color_wheels_rgb` performs the transformation below.

## Interface

`input: T[S,C]`, `zones: Float32/64[S,3]`, `controls: Float64[3,3,2]` [ev,offset] by zone and RGB component -> `values: T[S,C]`.

Select linear RGB. Zones use GRD-16A conventions. Suggested controls zero; offsets are in source units.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

For each component, e_c=sum(w_z*ev[z,c])/sum(w_z), o_c=sum(w_z*offset[z,c])/sum(w_z), y_c=x_c*exp2(e_c)+o_c. A component with all-zero controls copies.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

A +1 red EV in a one-hot zone doubles red alone. All-zero controls preserve RGB and alpha.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
