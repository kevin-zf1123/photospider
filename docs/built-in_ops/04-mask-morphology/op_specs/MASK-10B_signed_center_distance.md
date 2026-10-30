---
spec_schema_version: 1
id: MASK-10B
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-10
function: signed_center_distance
oracle_entry: signed_center_distance
registered_operation_keys:
  - mask.signed_center_distance_strict
---

# MASK-10B: Signed center-to-opposite-class distance

Inherits the [family contract](MASK-10_distance_transform_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.signed_center_distance_strict`.

input: Binary[H,W] -> distance: Float32/64[H,W]

Static parameters: metric,sy,sx,output_dtype,exterior; no feature or squared parameter. Authoring recommendation: exterior=background. Both none and background are supported; the node must explicitly serialize exterior.

## Mathematics

Foreground uses negative distance to background; background positive distance to foreground; no-feature -Inf for foreground and +Inf for background.

complete-input Binary input for any nonempty output demand.

No-feature results use the family infinity rule. No valid output is present.

## Acceptance

input=[0,1], unit spacing,none -> [+1,-1]. Full 1x1 with exterior background -> -1.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
