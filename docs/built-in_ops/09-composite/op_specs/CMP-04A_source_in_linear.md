---
spec_schema_version: 1
id: CMP-04A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: CMP-04
function: source_in_linear
execution: tile
proposed_operation_keys:
  - composite.source_in_linear
---

# CMP-04A: Source in in linear RGB/Gray

Inherit the [shared contract](COM_common_contract.md) and [CMP contract](CMP_coverage_contract.md). The proposed basic tile operator is `composite.source_in_linear`.

## Interface

`source: T[S,C]`, `background: T[S,C]` -> `image: T[S,C]`.

Matching linear RGB/Gray with explicit basis, white and reference units. Both images have matching geometry and selected color/alpha slots; alpha is in [0,1].

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

With source alpha a and background alpha b, Fs=b, Fb=0. Apply A=Fs*a+Fb*b and P=Fs*a*S+Fb*b*B; return straight color P/A when A>0 and positive-zero color when A=0.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Color output uses selected source colors and both alpha samples at that coordinate; the other image color payload is outside its mathematical support. Alpha output uses alpha alone. Unselected output slots copy the background independently.

## Acceptance

Use half-transparent red over half-transparent blue and compare the family analytic table. Verify alpha-only requests and nonzero ROI/tile coordinates.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
