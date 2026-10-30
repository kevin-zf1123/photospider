---
spec_schema_version: 1
id: GRD-05A
parent_id: GRD-05
function: curves_linear
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.curves_linear
---

# GRD-05A: Component curves with linear interpolation

Inherits the [family contract](GRD-05_curves_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.curves_linear` performs the transformation below.

## Interface

`input: T[S,C]`, `knots: Float64[K]`, `ordinates: Float32/64[K,Csel]` -> `values: T[S,C]`.

2<=K<=65536; knots strictly increase. Static edge=reject|clamp|linear_extrapolate, default reject. Each selected slot has its own ordinate column.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

At a knot select the ordinate. Between k[i] and k[i+1], t=(x-k[i])/(k[i+1]-k[i]) and y=(1-t)*v[i]+t*v[i+1]. Clamp selects an endpoint; extrapolation uses the endpoint secant. The reject domain is [k[0],k[K-1]].

## Behavior

Each output uses the source query at that coordinate and the contributing control-table entries. The complete auxiliary table is an admitted static support superset. A source edit is local; table edits affect observations whose declared support includes the table. Empty and bypass-only requests have no table payload dependency.

## Acceptance

Knots [0,1], ordinates [0,2] map 0.25 to 0.5. Test knot selection and both extrapolation sides.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
