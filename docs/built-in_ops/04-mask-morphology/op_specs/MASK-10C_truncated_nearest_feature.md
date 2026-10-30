---
spec_schema_version: 1
id: MASK-10C
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-10
function: truncated_nearest_feature
oracle_entry: truncated_nearest_feature
registered_operation_keys:
  - mask.truncated_nearest_feature_strict
---

# MASK-10C: Local truncated nearest-feature distance

Inherits the [family contract](MASK-10_distance_transform_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.truncated_nearest_feature_strict`.

input: Binary[H,W] -> distance: Float32/64[H,W], within: Binary[H,W]

Static parameters: A parameters plus limit>0 finite Float64. Authoring recommendation: exterior=none. Both none and background are supported; the node must explicitly serialize exterior.

## Mathematics

Exact radius search, saturate to limit (or limit²), equality counted within; no nearest coordinate output.

Support: exact closed metric-radius stencil around Q, clipped input support.

Binary flags use the associated value output floating dtype and exact 0/1.

## Acceptance

input=[1,0,0], limit=1,l2,foreground,none -> distance=[0,1,1], within=[1,1,0].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
