---
spec_schema_version: 1
id: CMP-02A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: CMP-02
function: source
execution: tile
proposed_operation_keys:
  - composite.source
---

# CMP-02A: Select source

Inherit the [shared contract](COM_common_contract.md) and [CMP contract](CMP_coverage_contract.md). The proposed basic tile operator is `composite.source`.

## Interface

`source: T[S,C]` -> `image: T[S,C]`.

Linear or encoded color and any auxiliary slots retain their supplied interpretation.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Copy every requested source sample bit-for-bit.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload.

## Acceptance

Preserve hidden colors, alpha, AOV values, signed zero and NaN payloads in copied slots.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
