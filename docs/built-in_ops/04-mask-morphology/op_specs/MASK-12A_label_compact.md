---
spec_schema_version: 1
id: MASK-12A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-12
function: label_compact
oracle_entry: label_compact
registered_operation_keys:
  - mask.label_compact_strict
---

# MASK-12A: Four/eight-connected compact component labels

Inherits the [family contract](MASK-12_components_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.label_compact_strict`.

input: Binary[H,W] -> labels: Int64[H,W]

Static parameters: connectivity 4|8; maximum_count Int64 in [0,N]. Optional `max_work` as in MASK-common.

## Mathematics

Compact first-pixel order, background 0, exact whole-component closure.

complete-input input; labels ROI remains globally numbered.

## Acceptance

binary [[1,0],[0,1]], connectivity4 -> [[1,0],[0,2]]; connectivity8 -> [[1,0],[0,1]].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
