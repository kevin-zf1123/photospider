---
spec_schema_version: 1
id: GRD-23A
parent_id: GRD-23
function: select_color_region
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.select_color_region
---

# GRD-23A: Soft chromatic-region selection

Inherits the [family contract](GRD-23_selective_color_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.select_color_region` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[11]` [h_center,h_inner,h_outer,c0,c1,c2,c3,l0,l1,l2,l3] -> `mask: Float64[S,1]`.

Select CIELCh or OKLCh with hue_unit=pi_multiple. 0<=h_inner<h_outer<=1; 0<=c0<c1<=c2<c3; l0<l1<=l2<l3.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Use smooth from GRD-18A. d=abs(((h-h_center+1) mod 2)-1). Define h_weight=1-smooth((d-h_inner)/(h_outer-h_inner)), c_weight=smooth((C-c0)/(c1-c0))*(1-smooth((C-c2)/(c3-c2))), l_weight=smooth((L-l0)/(l1-l0))*(1-smooth((L-l2)/(l3-l2))). Return their product. C=0 returns zero.

## Behavior

Each mask sample uses the selected native tuple. Its Float64 value lies in [0,1]. Selections for several corrections are derived from the same original image.

## Acceptance

At the hue center and inside both plateaus, return 1. Outside any outer range return 0. Gray returns 0.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
