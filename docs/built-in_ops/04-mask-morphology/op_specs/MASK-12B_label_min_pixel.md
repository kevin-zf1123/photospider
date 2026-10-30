---
spec_schema_version: 1
id: MASK-12B
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-12
function: label_min_pixel
oracle_entry: label_min_pixel
registered_operation_keys:
  - mask.label_min_pixel_strict
---

# MASK-12B: Four/eight-connected MinPixel labels

Inherits the [family contract](MASK-12_components_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.label_min_pixel_strict`.

input: Binary[H,W] -> labels: Int64[H,W]

Static parameters: connectivity 4|8; maximum_count Int64 in [0,N]. Optional `max_work` as in MASK-common.

## Mathematics

Each component gets 1+its minimum global row-major index.

complete-input input.

## Acceptance

binary [[1,0],[0,1]], connectivity4 -> [[1,0],[0,4]].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
