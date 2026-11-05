---
spec_schema_version: 1
id: NBL-01B
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: NBL-01
function: hue_oklab
execution: tile
proposed_operation_keys:
  - blend.hue_oklab
---

# NBL-01B: Hue blend in oklab

Inherit the [shared contract](COM_common_contract.md) and [NBL contract](NBL_nonseparable_contract.md). The proposed basic tile operator is `blend.hue_oklab`.

## Interface

`background: T[S,3]`, `source: T[S,3]`, `neutral_threshold: T[1]` -> `color: T[S,3]`.

Matching native OKLab coordinates and reference. neutral_threshold>=0 in native opponent units; suggested 0.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Use the native hue equation in the [NBL contract](NBL_nonseparable_contract.md).

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Output is a color-only tuple. The family equations define per-component data and control dependencies; model conversion and coverage integration are explicit surrounding operators.

## Acceptance

Evaluate these samples in the member's native coordinates. Bind `neutral_threshold=0` for every row.

| Case | Background B | Source S | Expected output |
| --- | --- | --- | --- |
| Identity | (0.5,0.25,0.125) | (0.5,0.25,0.125) | (0.5,0.25,0.125) |
| Property replacement | (0.5,3,4) | (0.25,0,2) | (0.5,0,5) |
| Neutral source | (0.5,3,4) | (0.25,0,0) | (0.5,3,4) |
| Zero background chroma | (0.5,0,0) | (0.25,0,2) | (0.5,+0,+0) |

The output L coordinate copies background L. A neutral source copies both background opponents; a nonneutral source supplies hue while retaining background chroma. Generated opponents for zero background chroma are positive zero.

Use the [COM comparison rules](COM_common_contract.md#numerical-target-and-strict-reference) for both Float32 and Float64. Fractions in the tables denote exact reference values before output rounding. Compare continuous results within the inherited tolerances; compare copied coordinates and discrete branch decisions exactly. Repeat at identical coordinates in whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
