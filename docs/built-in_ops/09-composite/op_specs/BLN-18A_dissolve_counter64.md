---
spec_schema_version: 1
id: BLN-18A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: BLN-18
function: dissolve_counter64
execution: tile
proposed_operation_keys:
  - blend.dissolve_counter64
---

# BLN-18A: Seeded coverage dissolve

Inherit the [shared contract](COM_common_contract.md) and [BLN contract](BLN_color_function_contract.md). The proposed basic tile operator is `blend.dissolve_counter64`.

## Interface

`source: T[S,C]`, `background: T[S,C]` -> `image: T[S,C]`.

Matching RGB/Gray images with alpha; linear or encoded interpretation is explicit. Static seed:Int64=0, frame:Int64=0; x/y are full-image integer coordinates.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Compute U=counter64(seed,x,y,frame) as defined in the BLN contract. If U/2^64 < source alpha, select source color and alpha 1; otherwise select background color and alpha. Compare the integer-rational threshold without an intervening floating RNG conversion.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Source alpha controls the branch. Source/background color dependencies follow that branch; unrelated output slots copy background. The same seed, frame and coordinate give the same decision regardless of tile partition.

## Acceptance

Source alpha 0 selects background; alpha 1 selects source with alpha 1. A nonzero ROI retains the global pattern.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
