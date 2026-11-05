---
spec_schema_version: 1
id: BLN-17C
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: BLN-17
function: darker_color_oklab_l
execution: tile
proposed_operation_keys:
  - blend.darker_color_oklab_l
---

# BLN-17C: Darker whole color by oklab_l

Inherit the [shared contract](COM_common_contract.md) and [BLN contract](BLN_color_function_contract.md). The proposed basic tile operator is `blend.darker_color_oklab_l`.

## Interface

`background: T[S,3]`, `source: T[S,3]` -> `color: T[S,3]`.

Inputs are native OKLab in the same explicit interpretation.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Use the stored first coordinate of each native model tuple. Select the complete background tuple when its measure is <= the source measure; otherwise select the complete source tuple. Ties select background. Selection copies the original tuple bits.

The [current implementation profile](BLN_color_function_contract.md#current-bln-11-through-bln-20-profile)
specifies the native arithmetic stages, exact decisions, parameters and prototype.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Y selection requires both full RGB tuples; native-L selection needs both first coordinates as control and the selected components as data. Output remains in the same input model.

## Acceptance

A selected tuple is copied as a unit, including signed zeros. For native model inputs, swapping only opponent coordinates does not affect which first-coordinate value wins.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
