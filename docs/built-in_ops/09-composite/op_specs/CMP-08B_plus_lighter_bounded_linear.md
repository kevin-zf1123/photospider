---
spec_schema_version: 1
id: CMP-08B
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: CMP-08
function: plus_lighter_bounded_linear
execution: tile
proposed_operation_keys:
  - composite.plus_lighter_bounded_linear
---

# CMP-08B: Plus lighter bounded in linear RGB/Gray

Inherit the [shared contract](COM_common_contract.md) and [CMP contract](CMP_coverage_contract.md). The proposed basic tile operator is `composite.plus_lighter_bounded_linear`.

## Interface

`source: T[S,C]`, `background: T[S,C]` -> `image: T[S,C]`.

Matching linear RGB/Gray with explicit basis, white and reference units. Both images have matching geometry and selected color/alpha slots; alpha is in [0,1]. Selected colors are in [0,1].

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Set A=min(1,a+b) and P=min(1,a*S+b*B) componentwise. Return P/A for A>0, otherwise positive-zero color, together with A.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Color output uses both selected source colors and both alpha samples at that coordinate. Alpha output uses alpha alone. Unselected output slots copy the background independently.

## Acceptance

Use half-transparent red over half-transparent blue and compare the family analytic table. Verify alpha-only requests and nonzero ROI/tile coordinates.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
