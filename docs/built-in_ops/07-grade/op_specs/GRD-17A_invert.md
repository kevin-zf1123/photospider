---
spec_schema_version: 1
id: GRD-17A
parent_id: GRD-17
function: invert
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.invert
---

# GRD-17A: Reflect a numeric range

Inherits the [family contract](GRD-17_invert_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.invert` performs the transformation below.

## Interface

`input: T[S,C]`, `bounds: Float64[Csel,2]` [lo,hi] -> `values: T[S,C]`.

lo<hi. Suggested bounds [0,1]. Input may extend beyond these bounds.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

y=lo+hi-x. Results remain unclipped. Applying twice is an algebraic identity subject to floating error.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

[0,0.25,1,2] with [0,1] gives [1,0.75,0,-1].

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
