---
spec_schema_version: 1
id: MASK-12D
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-12
function: component_areas
oracle_entry: component_areas
registered_operation_keys:
  - mask.component_areas_strict
---

# MASK-12D: Associated component area table

Inherits the [family contract](MASK-12_components_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.component_areas_strict`.

labels: Int64[H,W] -> area_index: complete Result rows:Int64[N,2] plus count:Int64[1], N=H*W

Static parameters: None. The workflow pairs the standalone area-index Result with the exact source Labels ObjectId and spatial basis as an unchecked caller premise. Optional `max_work` as in MASK-common.

## Mathematics

Count each positive ID exactly and emit ascending [id,area]; effective count K in an Int64[1] payload.

complete-input labels; atomic table publication.

## Acceptance

labels [[0,7],[7,42]] -> rows [[7,2],[42,1]]. All background -> count=0 with zero-filled static rows[N,2]. The listed nonempty rows are the effective prefix only.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
