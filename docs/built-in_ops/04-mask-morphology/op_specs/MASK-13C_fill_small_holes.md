---
spec_schema_version: 1
id: MASK-13C
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-13
function: fill_small_holes
oracle_entry: fill_small_holes
registered_operation_keys:
  - mask.fill_small_holes_strict
---

# MASK-13C: Fill enclosed holes up to an inclusive size

Inherits the [family contract](MASK-13_cleanup_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.fill_small_holes_strict`.

input: Binary[H,W] -> values: same dtype/shape

Static parameters: foreground_connectivity 4|8; maximum_area Int64>=0. Optional `max_work` as in MASK-common.

## Mathematics

Fill enclosed zero components with area<=maximum_area; never fill edge-connected exterior.

complete-input Binary input.

## Acceptance

3x3 ring center0: maximum_area0 unchanged,1 filled; an edge notch is never filled.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
