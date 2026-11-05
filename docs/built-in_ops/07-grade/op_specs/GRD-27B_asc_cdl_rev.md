---
spec_schema_version: 1
id: GRD-27B
parent_id: GRD-27
function: asc_cdl_rev
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.asc_cdl_rev
---

# GRD-27B: ASC CDL reverse with clamping

Inherits the [family contract](GRD-27_asc_cdl_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.asc_cdl_rev` performs the transformation below.

## Interface

`input: T[S,C]`, `sop: Float64[3,3]` with RGB rows [slope,offset,power], `saturation: Float64[1]` -> `values: T[S,C]`.

Select a complete RGB group in explicitly declared linear or encoded coordinates. Suggested SOP rows [1,0,1], saturation=1. The CDL luma weights are (0.2126,0.7152,0.0722), independent of the RGB basis. slope>0,power>0,saturation>0.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

v=clamp(x,0,1); L=dot(w,v); u_c=L+(v_c-L)/sat; y_c=clamp((pow(clamp(u_c,0,1),1/p_c)-o_c)/s_c,0,1).

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Default controls clamp extended input. Values removed by the forward clamp are not recovered.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
