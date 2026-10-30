---
spec_schema_version: 1
id: KEY-09A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: KEY-09
function: edge_extend_nearest
execution: tile
proposed_operation_keys:
  - keying.edge_extend_nearest
---

# KEY-09A: Extend original edge colors

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.edge_extend_nearest`.

## Interface

`image: T[H,W,C]`, `opaque_threshold: T[1]` -> `image: T[H,W,C]`.

Select straight RGB and alpha. 0<opaque_threshold<=1, suggested 1. Required radius:Int64 0..4096 in pixels.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Where alpha<threshold choose an original donor with alpha>=threshold in the integer disk dx^2+dy^2<=radius^2, minimizing (distance^2,y,x). Copy donor RGB and retain target alpha. With no donor, copy target. Original pixels above threshold copy.

## Behavior

H(radius) support on original colors and alpha. Donors never include newly extended samples. Other channels copy target.

## Acceptance

Radius zero is identity. Extending opaque red into transparent neighbors changes their hidden RGB while retaining their alpha.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
