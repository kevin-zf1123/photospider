---
spec_schema_version: 1
id: GRD-25C
parent_id: GRD-25
function: histogram_apply
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.histogram_apply
---

# GRD-25C: Apply a histogram map

Inherits the [family contract](GRD-25_histogram_equalize_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.histogram_apply` performs the transformation below.

## Interface

`feature: T[H,W]`, `map: Float64[B]`, `identity: UInt8[1]` -> `values: T[H,W]`.

2<=B<=65536. Map is nondecreasing in [0,1]; identity is 0/1; edge=reject|clamp defaults clamp.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Process the source domain as GRD-25A. If identity=1, copy u; otherwise return map[min(B-1,floor(B*u))]. The lookup is a step map.

## Behavior

Local feature plus flag and lookup entry; the full map is an admitted support superset. identity=1 has no map payload dependency.

## Acceptance

An identity flag preserves an in-domain sample such as 0.123. Map [0,1] maps 0.25 to 0 and 0.75 to 1.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
