---
spec_schema_version: 1
id: NBL-04C
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: NBL-04
function: luminosity_w3c
execution: tile
proposed_operation_keys:
  - blend.luminosity_w3c
---

# NBL-04C: Lightness blend in w3c

Inherit the [shared contract](COM_common_contract.md) and [NBL contract](NBL_nonseparable_contract.md). The proposed basic tile operator is `blend.luminosity_w3c`.

## Interface

`background: T[S,3]`, `source: T[S,3]` -> `color: T[S,3]`.

Matching bounded RGB coordinates, with an explicitly declared common linear or encoded space.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Use the lightness construction from the W3C subsection of the [NBL contract](NBL_nonseparable_contract.md).

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Output is a color-only tuple. The family equations define per-component data and control dependencies; model conversion and coverage integration are explicit surrounding operators.

## Acceptance

Compare bounded primaries and the family W3C analytic cases.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
