---
spec_schema_version: 1
id: CMP-03F
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: CMP-03
function: source_over_blended_color_encoded
execution: tile
proposed_operation_keys:
  - composite.source_over_blended_color_encoded
---

# CMP-03F: Source over with supplied blended color in encoded coordinates

Inherit the [shared contract](COM_common_contract.md) and [CMP contract](CMP_coverage_contract.md). The proposed basic tile operator is `composite.source_over_blended_color_encoded`.

## Interface

`source: T[S,C]`, `background: T[S,C]`, `blended: T[S,K]` -> `image: T[S,C]`.

All selected colors and blended values are in the same explicitly described encoded RGB/Gray. K is the selected color count, 1 or 3. Alpha is in [0,1].

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

A=a+b-a*b; P=(1-a)*b*B+(1-b)*a*S+a*b*F, where F is the supplied stored blended color. Return P/A for A>0, otherwise +0 color, and output A. This primitive performs coverage integration of an already computed color function.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Color output uses both selected source colors and both alpha samples at that coordinate. Alpha output uses alpha alone. Unselected output slots copy the background independently. F is needed for color output only.

## Acceptance

With F equal to source colors, agree with ordinary source-over within native tolerance. With opaque inputs, output F and alpha 1.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
