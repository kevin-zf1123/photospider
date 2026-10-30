---
spec_schema_version: 1
id: NBL-03B
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: NBL-03
function: color_oklab
execution: tile
proposed_operation_keys:
  - blend.color_oklab
---

# NBL-03B: Color blend in oklab

Inherit the [shared contract](COM_common_contract.md) and [NBL contract](NBL_nonseparable_contract.md). The proposed basic tile operator is `blend.color_oklab`.

## Interface

`background: T[S,3]`, `source: T[S,3]` -> `color: T[S,3]`.

Matching native OKLab coordinates and reference.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Use the native color equation in the [NBL contract](NBL_nonseparable_contract.md).

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Output is a color-only tuple. The family equations define per-component data and control dependencies; model conversion and coverage integration are explicit surrounding operators.

## Acceptance

Check copied first/opponent coordinates, neutral source and neutral background. Swapping lightness copies source l/L and background a,b exactly.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
