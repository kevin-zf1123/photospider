---
spec_schema_version: 1
id: MASK-12G
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-12
function: filter_area_index
oracle_entry: filter_area_index
registered_operation_keys:
  - mask.filter_area_index_strict
---

# MASK-12G: Filter labels through their associated area index

Inherits the [family contract](MASK-12_components_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.filter_area_index_strict`.

labels: Int64[H,W], area_index: complete associated Result -> mask: Binary[H,W]

Static parameters: output_dtype: float32|float64; minimum_area Int64>=1. Optional `max_work` as in MASK-common.

## Mathematics

Consume the correctly associated table under its count/ID/area premises; keep area>=minimum_area.

complete-input labels and the correctly paired area table, including its effective rows.

## Acceptance

labels [[0,7],[7,42]], areas [[7,2],[42,1]], minimum_area2 -> [[0,1],[1,0]]; foreign ObjectId inputs are outside the caller association premise and have no runtime rejection guarantee.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
