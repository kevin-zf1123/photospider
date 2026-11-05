---
spec_schema_version: 1
id: GRD-29D
parent_id: GRD-29
function: dither_ordered_map
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.dither_ordered_map
---

# GRD-29D: Dither with an explicit threshold map

Inherits the [family contract](GRD-29_dither_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.dither_ordered_map` performs the transformation below.

## Interface

`input: T[H,W,C]`, `thresholds: Float64[My,Mx]` -> `values: T[H,W,C]`.

1<=Mx,My<=4096; thresholds in (0,1). levels,origin,channel_phase,edge as GRD-29A, without order.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Lookup a repeating threshold at absolute coordinates using the GRD-29A origin/phase rule and Mx/My periods. Apply the family uniform quantization equation.

## Behavior

Each output uses the source query at that coordinate and the contributing control-table entries. The complete auxiliary table is an admitted static support superset. A source edit is local; table edits affect observations whose declared support includes the table. Empty and bypass-only requests have no table payload dependency.

## Acceptance

Supplying the Bayer threshold matrix reproduces GRD-29A. Repeated equal thresholds are legal. Check negative-coordinate wrapping.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
