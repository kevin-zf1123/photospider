---
spec_schema_version: 1
id: GRD-23C
parent_id: GRD-23
function: select_neutral_region
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.select_neutral_region
---

# GRD-23C: Soft neutral-region selection

Inherits the [family contract](GRD-23_selective_color_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.select_neutral_region` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[6]` [c_inner,c_outer,l0,l1,l2,l3] -> `mask: Float64[S,1]`.

Select CIELAB or OKLab. 0<=c_inner<c_outer; l0<l1<=l2<l3. Controls are required and use native units.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

C=hypot(a,b). c_weight=1-smooth((C-c_inner)/(c_outer-c_inner)); l_weight=smooth((L-l0)/(l1-l0))*(1-smooth((L-l2)/(l3-l2))). Return c_weight*l_weight. Setting the lightness plateau around dark, middle or bright values selects black, gray or white regions respectively.

The native profile compares exact binary64 squared chroma with squared thresholds near endpoints and in narrow transitions. Interior transition coordinates use the rationalized difference `(a*a+b*b-c_inner*c_inner)/((hypot(a,b)+c_inner)*(c_outer-c_inner))` with scaled evaluation. This preserves mathematical inner/outer endpoints without magnifying a rounded hypot error into a full mask transition.

## Behavior

E support on native L,a,b and controls. The result is a Float64 soft mask in [0,1] and can feed GRD-23B.

## Acceptance

At a=b=0 inside the lightness plateau return 1. At C>=c_outer return 0. Test both lightness transitions.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
