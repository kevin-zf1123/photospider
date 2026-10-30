---
spec_schema_version: 1
id: MASK-10D
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-10
function: truncated_signed_distance
oracle_entry: truncated_signed_distance
registered_operation_keys:
  - mask.truncated_signed_distance_strict
---

# MASK-10D: Local truncated signed center-distance

Inherits the [family contract](MASK-10_distance_transform_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.truncated_signed_distance_strict`.

input: Binary[H,W] -> distance: Float32/64[H,W], within: Binary[H,W]

Static parameters: B parameters plus limit>0 finite Float64. Authoring recommendation: exterior=background. Both none and background are supported; the node must explicitly serialize exterior.

## Mathematics

Signed opposite-class radius search; missing opposite feature saturates with the center-class sign.

Support: closed metric-radius input stencil plus center; no complete-input shortcut.

Binary flags use the associated value output floating dtype and exact 0/1.

## Acceptance

full 1x1, exterior none,limit=2 -> distance=-2,within=0; empty image membership -> +2,within=0.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
