---
spec_schema_version: 1
id: BLN-16B
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: BLN-16
function: hard_mix_sum
execution: tile
proposed_operation_keys:
  - blend.hard_mix_sum
---

# BLN-16B: Hard mix sum

Inherit the [shared contract](COM_common_contract.md) and [BLN contract](BLN_color_function_contract.md). The proposed basic tile operator is `blend.hard_mix_sum`.

## Interface

`background: T[S,K]`, `source: T[S,K]` -> `color: T[S,K]`.

K=1 for Gray or 3 for RGB, with matching explicit linear or encoded interpretation. Consumed values are in [0,1].

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Return 1 iff B+S>=1, otherwise 0.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Each color component uses the matching source/background components; normal reads source only. These color-only ports contain no alpha or effect mask.

## Acceptance

B=0,S=1 gives 1.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
