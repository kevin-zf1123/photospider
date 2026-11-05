---
spec_schema_version: 1
id: BLN-19A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: BLN-19
function: emission_add_straight
execution: tile
proposed_operation_keys:
  - blend.emission_add_straight
---

# BLN-19A: Add a linear color signal with retained alpha

Inherit the [shared contract](COM_common_contract.md) and [BLN contract](BLN_color_function_contract.md). The proposed basic tile operator is `blend.emission_add_straight`.

## Interface

`background: T[S,C]`, `emission: T[S,3]`, `gain: T[1]` -> `image: T[S,C]`.

Select linear RGB and alpha; emission has the same basis/reference units. gain>=0; suggested gain=0.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

color=background_color+gain*emission. Copy background alpha and unselected channels. gain=0 copies the original image.

The [current implementation profile](BLN_color_function_contract.md#current-bln-11-through-bln-20-profile)
specifies the native arithmetic stages, exact decisions, parameters and prototype.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. This is a straight-color adjustment with unchanged coverage. It does not represent independently visible zero-coverage Layer emission.

## Acceptance

RGB=(0.25,0,0), emission=(0.5,0,0), gain=1 yields red 0.75 with unchanged alpha.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
