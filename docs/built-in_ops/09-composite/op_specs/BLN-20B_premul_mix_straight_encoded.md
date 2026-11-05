---
spec_schema_version: 1
id: BLN-20B
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: BLN-20
function: premul_mix_straight_encoded
execution: tile
proposed_operation_keys:
  - blend.premul_mix_straight_encoded
---

# BLN-20B: Coverage-weighted image interpolation in encoded coordinates

Inherit the [shared contract](COM_common_contract.md) and [BLN contract](BLN_color_function_contract.md). The proposed basic tile operator is `blend.premul_mix_straight_encoded`.

## Interface

`first: T[S,C]`, `second: T[S,C]`, `mix: T[S,1]` -> `image: T[S,C]`.

Matching explicitly encoded RGB/Gray images. Each has selected alpha. mix is a same-grid field in [0,1]; use an explicit constant field for uniform interpolation.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

For t=mix, t=0 copies first and t=1 selects second color/alpha. Otherwise A=(1-t)*a+t*b and P=(1-t)*a*C_first+t*b*C_second. Return P/A for A>0, otherwise +0 color, with A. Alpha and color coordinates use the member working domain.

The [current implementation profile](BLN_color_function_contract.md#current-bln-11-through-bln-20-profile)
specifies the native arithmetic stages, exact decisions, parameters and prototype.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Unselected output slots copy first independently, including at t=1. Color requires the matching colors, alphas and t; alpha requires alphas and t only.

## Acceptance

Equal alphas are retained. With opaque red/blue and t=0.5, output (0.5,0,0.5,1) in the declared domain. Zero t preserves first hidden color.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
