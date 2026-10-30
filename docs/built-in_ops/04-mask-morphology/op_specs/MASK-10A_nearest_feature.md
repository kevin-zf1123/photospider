---
spec_schema_version: 1
id: MASK-10A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-10
function: nearest_feature
oracle_entry: nearest_feature
registered_operation_keys:
  - mask.nearest_feature_strict
---

# MASK-10A: Nearest discrete feature and distance

Inherits the [family contract](MASK-10_distance_transform_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.nearest_feature_strict`.

input: Binary[H,W] -> distance: Float32/64[H,W], nearest: Int64[2,H,W]

Static parameters: metric,sy,sx,output_dtype,exterior,feature,squared, as family. Authoring recommendation: exterior=none. Both none and background are supported; the node must explicitly serialize exterior.

## Mathematics

Exact nearest site and lexicographic tie; distance is RN_T(metric distance), or RN_T(squared L2); explicit no-feature +Inf.

complete-input Binary input for any nonempty output demand; buffers are separately requestable, shared site choice.

No-feature results use the family infinity rule. No valid output is present.

## Acceptance

input=[1,0,1], feature=foreground,exterior=none,l2 -> distances=[0,1,0]; middle nearest=(0,0). All-zero foreground search gives +Inf and nearest=(-1,-1).

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
