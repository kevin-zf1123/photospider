---
spec_schema_version: 1
id: BLN-17B
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: BLN-17
function: lighter_color_linear_y
execution: tile
proposed_operation_keys:
  - blend.lighter_color_linear_y
---

# BLN-17B: Lighter whole color by linear_y

Inherit the [shared contract](COM_common_contract.md) and [BLN contract](BLN_color_function_contract.md). The proposed basic tile operator is `blend.lighter_color_linear_y`.

## Interface

`background: T[S,3]`, `source: T[S,3]` -> `color: T[S,3]`.

Inputs are linear RGB with the same explicit RGB basis and white. The basis Y row is resolved in Float64; it may contain signed coefficients.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Compute the Float64 Y dot product for each RGB tuple. Select the complete background tuple when its measure is >= the source measure; otherwise select the complete source tuple. Ties select background. Selection copies the original tuple bits.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Y selection requires both full RGB tuples; native-L selection needs both first coordinates as control and the selected components as data. Output remains in the same input model.

## Acceptance

A selected tuple is copied as a unit, including signed zeros. For native model inputs, swapping only opponent coordinates does not affect which first-coordinate value wins.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
