---
spec_schema_version: 1
id: GRD-27D
parent_id: GRD-27
function: asc_cdl_rev_no_clamp
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.asc_cdl_rev_no_clamp
---

# GRD-27D: ASC CDL reverse without clamping

Inherits the [family contract](GRD-27_asc_cdl_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.asc_cdl_rev_no_clamp` performs the transformation below.

## Interface

`input: T[S,C]`, `sop: Float64[3,3]` with RGB rows [slope,offset,power], `saturation: Float64[1]` -> `values: T[S,C]`.

Select a complete RGB group in explicitly declared linear or encoded coordinates. Suggested SOP rows [1,0,1], saturation=1. The CDL luma weights are (0.2126,0.7152,0.0722), independent of the RGB basis. slope>0,power>0,saturation>0.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

L=dot(w,x); u_c=L+(x_c-L)/sat; v_c=u_c when u_c<0, otherwise pow(u_c,1/p_c); y_c=(v_c-o_c)/s_c. Neutral controls copy.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Test a forward/reverse round trip under positive slopes and saturation with floating tolerance.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
