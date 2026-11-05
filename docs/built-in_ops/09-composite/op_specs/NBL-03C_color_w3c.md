---
spec_schema_version: 1
id: NBL-03C
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: NBL-03
function: color_w3c
execution: tile
proposed_operation_keys:
  - blend.color_w3c
---

# NBL-03C: Color blend in w3c

Inherit the [shared contract](COM_common_contract.md) and [NBL contract](NBL_nonseparable_contract.md). The proposed basic tile operator is `blend.color_w3c`.

## Interface

`background: T[S,3]`, `source: T[S,3]` -> `color: T[S,3]`.

Matching bounded RGB coordinates, with an explicitly declared common linear or encoded space.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Use the color construction from the W3C subsection of the [NBL contract](NBL_nonseparable_contract.md).

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Output is a color-only tuple. The family equations define per-component data and control dependencies; model conversion and coverage integration are explicit surrounding operators.

## Acceptance

Evaluate these bounded RGB samples in the explicitly selected working domain.

| Case | Background B | Source S | Expected output |
| --- | --- | --- | --- |
| Identity | (0.5,0.25,0.125) | (0.5,0.25,0.125) | (0.5,0.25,0.125) |
| Color replacement | (0.5,0.5,0.5) | (0.75,0.25,0.5) | (329/400,129/400,229/400) |
| Lower-bound correction | (1,0,0) | (0,1,0) | (0,30/59,0) |

Check the full three-coordinate construction, including tied extrema and gray tuples. Outputs stay in `[0,1]`; each requested output coordinate depends on both full RGB inputs.

Use the [COM comparison rules](COM_common_contract.md#numerical-target-and-strict-reference) for both Float32 and Float64. Fractions in the tables denote exact reference values before output rounding. Compare continuous results within the inherited tolerances; compare copied coordinates and discrete branch decisions exactly. Repeat at identical coordinates in whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
