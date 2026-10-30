---
spec_schema_version: 1
id: GRD-06A
parent_id: GRD-06
function: gradient_map
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.gradient_map
---

# GRD-06A: Map a feature through an RGB ramp

Inherits the [family contract](GRD-06_gradient_map_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.gradient_map` performs the transformation below.

## Interface

`feature: T[S,1]`, `knots: Float64[K]`, `colors: Float32/64[K,3]` -> `values: T[S,3]`.

2<=K<=65536; strictly increasing knots. Required destination RGB description; edge=reject|clamp, default clamp.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Apply the GRD-05A linear formula to each of the three color columns with the same query. Interpolation occurs in the explicitly specified RGB coordinates. Output dtype is the feature dtype.

## Behavior

Each output uses the source query at that coordinate and the contributing control-table entries. The complete auxiliary table is an admitted static support superset. A source edit is local; table edits affect observations whose declared support includes the table. Empty and bypass-only requests have no table payload dependency.

## Acceptance

Ramp black at 0 and red at 1 maps 0.25 to (0.25,0,0). Out-of-range queries clamp to endpoint colors by default.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
