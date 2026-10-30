---
spec_schema_version: 1
id: MASK-12C
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-12
function: component_count
oracle_entry: component_count
registered_operation_keys:
  - mask.component_count_strict
---

# MASK-12C: Count declared positive label IDs

Inherits the [family contract](MASK-12_components_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.component_count_strict`.

labels: Int64[H,W] -> count: Int64[1]

Static parameters: None. Optional `max_work` as in MASK-common.

## Mathematics

Count distinct positive IDs under the nonnegative-label premise; no connectivity revalidation.

complete-input labels.

## Acceptance

labels [[0,7],[7,42]] -> count=2, not 42.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
