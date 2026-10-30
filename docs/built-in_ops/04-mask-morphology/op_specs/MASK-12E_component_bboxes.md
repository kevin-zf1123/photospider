---
spec_schema_version: 1
id: MASK-12E
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-12
function: component_bboxes
oracle_entry: component_bboxes
registered_operation_keys:
  - mask.component_bboxes_strict
---

# MASK-12E: Associated component bounding-box table

Inherits the [family contract](MASK-12_components_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.component_bboxes_strict`.

labels: Int64[H,W] -> bbox_index: complete Result rows:Int64[N,5] plus count:Int64[1], N=H*W

Static parameters: None. The workflow pairs the standalone bounding-box-index Result with the exact source Labels ObjectId and spatial basis as an unchecked caller premise. Optional `max_work` as in MASK-common.

## Mathematics

Ascending ID with [id,y0,x0,y1_exclusive,x1_exclusive].

complete-input labels; atomic table publication.

## Acceptance

labels [[0,7],[7,42]] -> [[7,0,0,2,2],[42,1,1,2,2]].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
