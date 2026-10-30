---
spec_schema_version: 1
id: MASK-13A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-13
function: fill_holes
oracle_entry: fill_holes
registered_operation_keys:
  - mask.fill_holes_strict
---

# MASK-13A: Fill every enclosed binary hole

Inherits the [family contract](MASK-13_cleanup_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.fill_holes_strict`.

input: Binary[H,W] -> values: same dtype/shape

Static parameters: foreground_connectivity 4|8; complementary background rule. Optional `max_work` as in MASK-common.

## Mathematics

Fill only zero components not connected to the canvas boundary.

complete-input Binary input.

## Acceptance

3x3 ring of ones with center0 -> full ones. All zeros -> all zeros, not full ones.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
