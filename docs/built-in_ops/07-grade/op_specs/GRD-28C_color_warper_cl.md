---
spec_schema_version: 1
id: GRD-28C
parent_id: GRD-28
function: color_warper_cl
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.color_warper_cl
---

# GRD-28C: Chroma-lightness warp

Inherits the [family contract](GRD-28_color_warper_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.color_warper_cl` performs the transformation below.

## Interface

`input: T[S,C]`, `chroma_axis: Float64[Nc]`, `lightness_axis: Float64[Nl]`, `displacement: Float64[Nc,Nl,2]` [dC,dL] -> `values: T[S,C]`.

CIELCh/OKLCh. 2..256 axis lengths; strictly increasing axes, chroma starts at 0. edge=reject|clamp defaults clamp; topology=allow|positive_jacobian defaults allow.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Bilinearly interpolate displacement at (C,L), clamping only lookup coordinates when requested. Add to original C,L; copy hue. At C=0 require dC=0, allowing dL to change gray brightness. Mapped vertex chroma is nonnegative. The family local Jacobian condition defines positive_jacobian.

## Behavior

Each output uses the source query at that coordinate and the contributing control-table entries. The complete auxiliary table is an admitted static support superset. A source edit is local; table edits affect observations whose declared support includes the table. Empty and bypass-only requests have no table payload dependency.

## Acceptance

Zero mesh is identity. Gray remains achromatic while its L may change. Hue is a bit-preserving bypass.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
