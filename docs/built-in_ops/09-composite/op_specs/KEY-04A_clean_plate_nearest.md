---
spec_schema_version: 1
id: KEY-04A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: KEY-04
function: clean_plate_nearest
execution: tile
proposed_operation_keys:
  - keying.clean_plate_nearest
---

# KEY-04A: Nearest known background plate

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.clean_plate_nearest`.

## Interface

`image: T[H,W,3]`, `known: UInt8[H,W]` -> `plate: T[H,W,3]`, `confidence: T[H,W]`.

Explicit common RGB interpretation. known is binary 0/1; at least one known pixel is needed for a plate value.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Known pixels copy the input. At each unknown pixel, select the original known pixel minimizing (squared Euclidean integer distance, donor_y, donor_x) and copy its RGB. confidence is 1 at originally known locations and 0 elsewhere.

## Behavior

W support on the binary known mask and original donor colors. A requested plate with no donor has NoSolution under the common error convention. confidence depends only on the local known flag and remains defined without donors.

## Acceptance

One known red pixel fills the plate with red; confidence is 1 only at that pixel. Equal-distance donor ties use y then x.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
