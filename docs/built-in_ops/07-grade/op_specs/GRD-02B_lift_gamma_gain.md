---
spec_schema_version: 1
id: GRD-02B
parent_id: GRD-02
function: lift_gamma_gain
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.lift_gamma_gain
---

# GRD-02B: Lift, gamma and gain

Inherits the [family contract](GRD-02_offset_gain_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.lift_gamma_gain` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[Csel,3]` with rows [lift,gamma,gain] -> `values: T[S,C]`.

Suggested rows [0,1,1]; gamma>0, gain>=0; lift is finite.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

`t=(1-lift)*x+lift; y=gain*spow(t,1/gamma)`, where spow(t,a)=sign(t)*abs(t)^a and spow(0,a)=0. Identity controls copy. The signed extension preserves the sign through the power.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Identity copies negative values. With lift=0, gamma=2, gain=1, x=-0.25 gives -0.5.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
