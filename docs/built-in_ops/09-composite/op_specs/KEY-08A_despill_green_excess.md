---
spec_schema_version: 1
id: KEY-08A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: KEY-08
function: despill_green_excess
execution: tile
proposed_operation_keys:
  - keying.despill_green_excess
---

# KEY-08A: Suppress green excess

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.despill_green_excess`.

## Interface

`foreground: T[S,C]`, `mask: T[S,1]`, `strength: T[1]` -> `foreground: T[S,C]`.

Select linear RGB and optional alpha/AOV slots. mask,strength in [0,1]; suggested strength=0.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

e=max(0,G-max(R,B)); G_out=G-strength*mask*e. Copy R,B,alpha and unrelated slots. A zero correction factor copies the source.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload.

## Acceptance

For RGB=(0.25,1,0.5), unit mask/strength gives G=0.5. Zero strength is identity.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
