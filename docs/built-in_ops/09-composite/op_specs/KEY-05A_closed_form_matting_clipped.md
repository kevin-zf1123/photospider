---
spec_schema_version: 1
id: KEY-05A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: KEY-05
function: closed_form_matting_clipped
execution: tile
proposed_operation_keys:
  - keying.closed_form_matting_clipped
---

# KEY-05A: Trimap-constrained closed-form matting

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.closed_form_matting_clipped`.

## Interface

`image: T[H,W,3]`, `trimap: UInt8[H,W]`, `epsilon: T[1]` -> `matte: T[H,W]`, `raw_residual: Float64[1]`.

Relative linear RGB. Labels 0=background, 1=unknown, 2=foreground. epsilon>0 in squared RGB units, suggested 1e-7. Static solve_atol:Float64=1e-12 and solve_rtol:Float64=1e-8, both >=0 with positive sum.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Use the clipped 3x3 matting Laplacian, hard constraints and native numerical solution defined in [KEY matting](KEY_matting_contract.md). Clamp solved alpha to [0,1]. Known labels produce exact 0/1. raw_residual is the infinity norm of the unknown-equation residual before clamping.

## Behavior

W support through the image, trimap and epsilon. Matting is a tile-output operation with global mathematical dependence. NoSolution and NotConverged are failure diagnostics as defined in the common contract.

## Acceptance

Fully known trimaps reproduce labels exactly with raw_residual=0. Small partially known fixtures compare to the exact rational system; retain known endpoints even when unknown values need clipping.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
