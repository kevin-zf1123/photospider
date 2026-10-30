---
spec_schema_version: 1
id: GRD-05B
parent_id: GRD-05
function: curves_pchip
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.curves_pchip
---

# GRD-05B: Component curves with PCHIP interpolation

Inherits the [family contract](GRD-05_curves_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.curves_pchip` performs the transformation below.

## Interface

`input: T[S,C]`, `knots: Float64[K]`, `ordinates: Float32/64[K,Csel]` -> `values: T[S,C]`.

The shape, domain and edge parameters are those of [GRD-05A](GRD-05A_curves_linear.md).

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Use the slope construction and cubic Hermite polynomial of [CRV-01B](../../01-numeric/op_specs/CRV-01B_interpolate_pchip.md), evaluated under GRD native floating arithmetic. Interior slopes are zero for zero or opposite-sign secants and otherwise use the weighted harmonic mean. Endpoint limiting is inherited mathematically. With two knots use the secant. Outside the domain, linear_extrapolate uses the endpoint tangent.

## Behavior

Each output uses the source query at that coordinate and the contributing control-table entries. The complete auxiliary table is an admitted static support superset. A source edit is local; table edits affect observations whose declared support includes the table. Empty and bypass-only requests have no table payload dependency.

## Acceptance

Two knots reduce to linear interpolation. Monotone data remain within each segment endpoint range within floating tolerance; all knot values are selected directly.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
