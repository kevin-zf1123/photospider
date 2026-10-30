---
spec_schema_version: 1
id: CMP-08C
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: CMP-08
function: plus_raw_encoded
execution: tile
proposed_operation_keys:
  - composite.plus_raw_encoded
---

# CMP-08C: Plus raw in encoded RGB/Gray

Inherit the [shared contract](COM_common_contract.md) and [CMP contract](CMP_coverage_contract.md). The proposed basic tile operator is `composite.plus_raw_encoded`.

## Interface

`source: T[S,C]`, `background: T[S,C]` -> `color_mass: T[S,K]`, `mass: T[S,1]`.

Matching explicitly described encoded RGB/Gray, including the same transfer function. Both images have matching geometry and selected color/alpha slots; alpha is in [0,1].

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Return color_mass=a*S+b*B and mass=a+b. These outputs are raw weighted coordinates and weight mass, with mass in [0,2]; they are not a straight image or coverage alpha.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Each color_mass component depends on its corresponding colors and both alpha samples. mass depends only on alpha. Output K is 1 for Gray or 3 for RGB; no AOVs are emitted.

## Acceptance

Use half-transparent red over half-transparent blue and compare the family analytic table. Verify alpha-only requests and nonzero ROI/tile coordinates.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
