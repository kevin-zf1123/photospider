---
spec_schema_version: 1
id: BLN-13A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: BLN-13
function: vivid_light
execution: tile
proposed_operation_keys:
  - blend.vivid_light
---

# BLN-13A: Vivid light

Inherit the [shared contract](COM_common_contract.md) and [BLN contract](BLN_color_function_contract.md). The proposed basic tile operator is `blend.vivid_light`.

## Interface

`background: T[S,K]`, `source: T[S,K]` -> `color: T[S,K]`.

K=1 for Gray or 3 for RGB, with matching explicit linear or encoded interpretation. Consumed values are in [0,1].

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

burn(B,2*S) when S<=0.5, otherwise dodge(B,2*S-1), using the BLN-09/10 endpoint definitions.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Each color component uses the matching source/background components; normal reads source only. These color-only ports contain no alpha or effect mask.

## Acceptance

S=0.5 is neutral; retain dodge/burn endpoints.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
