---
spec_schema_version: 1
id: GRD-16A
parent_id: GRD-16
function: color_balance_rgb
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.color_balance_rgb
---

# GRD-16A: Tonal RGB color balance

Inherits the [family contract](GRD-16_color_balance_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.color_balance_rgb` performs the transformation below.

## Interface

`input: T[S,C]`, `zones: Float32/64[S,3]`, `offsets: Float64[3,3]` -> `values: T[S,C]`.

Select RGB in explicitly declared coordinates. Zone order is shadow,mid,highlight; weights are nonnegative with positive sum. Suggested offsets are zero.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

y_c=x_c+sum_z(w_z*offset[z,c])/sum_z(w_z). Zero offsets copy. Compute zones from the original feature or supply them independently.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

One-hot shadow weights apply only the shadow row. Scaling every zone weight by the same positive factor leaves the result unchanged.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
