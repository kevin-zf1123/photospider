---
spec_schema_version: 1
id: CMP-01A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: CMP-01
function: clear
execution: tile
proposed_operation_keys:
  - composite.clear
---

# CMP-01A: Clear an image

Inherit the [shared contract](COM_common_contract.md) and [CMP contract](CMP_coverage_contract.md). The proposed basic tile operator is `composite.clear`.

## Interface

`template: T[S,C]` -> `image: T[S,C]`.

The template supplies shape and dtype only. Color interpretation can be linear or encoded.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Return positive zero in every output slot, including alpha and any auxiliary channels.

## Behavior

Output depends only on the template descriptor. Every tile is generated without reading template samples.

## Acceptance

Every requested output bit is +0 for either dtype; a template containing NaN samples is not consumed.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
