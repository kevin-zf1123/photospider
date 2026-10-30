---
spec_schema_version: 1
id: KEY-07C
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: KEY-07
function: binary_fill_holes
execution: tile
proposed_operation_keys:
  - keying.binary_fill_holes
---

# KEY-07C: Fill enclosed binary holes

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.binary_fill_holes`.

## Interface

`mask: UInt8[H,W]` -> `mask: UInt8[H,W]`.

Binary values 0/1; connectivity is four-neighbor.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Mark all zero pixels connected to a full-image border zero by four-neighbor paths. Output 0 at those marked pixels and 1 elsewhere.

## Behavior

W support through border connectivity. The full-image border defines exterior; tile edges do not.

## Acceptance

All-zero stays zero; all-one stays one. A zero surrounded by ones becomes one; a diagonal-only contact does not connect it to exterior.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
