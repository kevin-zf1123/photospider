---
spec_schema_version: 1
id: NBL-02A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: NBL-02
function: chroma_lab_zero_hue
execution: tile
proposed_operation_keys:
  - blend.chroma_lab_zero_hue
---

# NBL-02A: Chroma blend in lab

Inherit the [shared contract](COM_common_contract.md) and [NBL contract](NBL_nonseparable_contract.md). The proposed basic tile operator is `blend.chroma_lab_zero_hue`.

## Interface

`background: T[S,3]`, `source: T[S,3]`, `neutral_threshold: T[1]` -> `color: T[S,3]`.

Matching native CIELAB with explicit common white; l=L*/100 and native a*,b*. neutral_threshold>=0 in native opponent units; suggested 0.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Use the native chroma equation in the [NBL contract](NBL_nonseparable_contract.md). Its neutral-background chroma policy is zero_hue: output the source chroma on the positive a axis.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Output is a color-only tuple. The family equations define per-component data and control dependencies; model conversion and coverage integration are explicit surrounding operators.

## Acceptance

Evaluate these samples in the member's native coordinates. Bind `neutral_threshold=0` for every row.

| Case | Background B | Source S | Expected output |
| --- | --- | --- | --- |
| Identity | (0.5,0.25,0.125) | (0.5,0.25,0.125) | (0.5,0.25,0.125) |
| Property replacement | (0.5,3,4) | (0.25,0,2) | (0.5,6/5,8/5) |
| Neutral background: `zero_hue` | (0.5,0,0) | (0.25,0,1) | (0.5,1,+0) |

The output L coordinate copies background L. A nonneutral background retains its hue and receives source chroma. Check the `zero_hue` branch separately at zero and positive thresholds, including exact equality in the squared-chroma predicate.

Use the [COM comparison rules](COM_common_contract.md#numerical-target-and-strict-reference) for both Float32 and Float64. Fractions in the tables denote exact reference values before output rounding. Compare continuous results within the inherited tolerances; compare copied coordinates and discrete branch decisions exactly. Repeat at identical coordinates in whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
