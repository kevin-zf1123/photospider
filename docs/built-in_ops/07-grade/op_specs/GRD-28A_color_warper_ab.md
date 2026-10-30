---
spec_schema_version: 1
id: GRD-28A
parent_id: GRD-28
function: color_warper_ab
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.color_warper_ab
---

# GRD-28A: Opponent-plane color warp

Inherits the [family contract](GRD-28_color_warper_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.color_warper_ab` performs the transformation below.

## Interface

`input: T[S,C]`, `u_axis: Float64[Nu]`, `v_axis: Float64[Nv]`, `displacement: Float64[Nu,Nv,2]` [da,db] -> `values: T[S,C]`.

CIELAB/OKLab; 2<=Nu,Nv<=256; axes strictly increase. edge=reject|clamp defaults clamp; topology=allow|positive_jacobian defaults allow; boundary=free|pinned_zero defaults free.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Lookup source (a,b) in the mesh and bilinearly interpolate displacement. Clamp changes only lookup coordinates; add displacement to original a,b and copy L. pinned_zero requires zero displacement at all boundary vertices. positive_jacobian is the local condition in the family contract.

## Behavior

Each output uses the source query at that coordinate and the contributing control-table entries. The complete auxiliary table is an admitted static support superset. A source edit is local; table edits affect observations whose declared support includes the table. Empty and bypass-only requests have no table payload dependency.

## Acceptance

A zero mesh is identity even outside the mesh under clamp. A constant displacement translates a,b. Lightness copies.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
