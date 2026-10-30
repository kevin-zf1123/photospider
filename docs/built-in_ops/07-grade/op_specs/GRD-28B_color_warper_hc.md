---
spec_schema_version: 1
id: GRD-28B
parent_id: GRD-28
function: color_warper_hc
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.color_warper_hc
---

# GRD-28B: Periodic hue-chroma warp

Inherits the [family contract](GRD-28_color_warper_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.color_warper_hc` performs the transformation below.

## Interface

`input: T[S,C]`, `hue_axis: Float64[Nu]`, `chroma_axis: Float64[Nv]`, `displacement: Float64[Nu,Nv,2]` [dh,dC] -> `values: T[S,C]`.

CIELCh/OKLCh; hue_unit=pi_multiple. 2..256 axis lengths; hue starts at 0, strictly increases below 2; chroma starts at 0 and strictly increases. edge=reject|clamp defaults clamp on C. topology=allow|positive_jacobian defaults allow.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Reduce hue modulo 2 for lookup, closing the last cell by repeating the first displacement column at h=2. Interpolate unwrapped dh,dC; add them to original h,C and copy L. Every displacement at C=0 is [0,0]; mapped C at all vertices is nonnegative. positive_jacobian uses unwrapped hue coordinates, including the closing cell.

## Behavior

Each output uses the source query at that coordinate and the contributing control-table entries. The complete auxiliary table is an admitted static support superset. A source edit is local; table edits affect observations whose declared support includes the table. Empty and bypass-only requests have no table payload dependency.

## Acceptance

Zero mesh preserves hue winding and gray. The seam is continuous. Positive-Jacobian mode has the family local condition and does not imply global invertibility.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
