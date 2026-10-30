---
spec_schema_version: 1
id: GRD-18B
parent_id: GRD-18
function: tonal_zone_grade
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.tonal_zone_grade
---

# GRD-18B: Exposure and offset by tonal zone

Inherits the [family contract](GRD-18_tonal_zones_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.tonal_zone_grade` performs the transformation below.

## Interface

`input: T[S,C]`, `zones: Float32/64[S,3]`, `controls: Float64[3,2]` [ev,offset] -> `values: T[S,C]`.

Linear RGB/Gray. Zone convention is GRD-16A. Suggested controls zero. EV uses stops; offset uses the input unit.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

e=sum(w_z*ev_z)/sum(w_z); o=sum(w_z*offset_z)/sum(w_z); y=x*exp2(e)+o. All-zero controls copy.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

One-hot shadow weights and shadow EV=1 double selected components. With all controls zero, preserve signed/HDR values.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
