---
spec_schema_version: 1
id: MASK-13B
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-13
function: remove_small
oracle_entry: remove_small
registered_operation_keys:
  - mask.remove_small_strict
---

# MASK-13B: Remove small foreground components

Inherits the [family contract](MASK-13_cleanup_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.remove_small_strict`.

input: Binary[H,W] -> values: same dtype/shape

Static parameters: foreground_connectivity 4|8; maximum_removed_area Int64>=0. Optional `max_work` as in MASK-common.

## Mathematics

Remove components with area<=maximum_removed_area, using exact pixel counts; retain larger components.

complete-input Binary input.

## Acceptance

a two-pixel component survives maximum_removed_area=1 and disappears at2; equality is removed. A zero threshold is an identity on valid Binary input.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
