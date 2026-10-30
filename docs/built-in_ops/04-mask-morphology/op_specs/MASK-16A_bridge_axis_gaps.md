---
spec_schema_version: 1
id: MASK-16A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D2_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-16
function: bridge_axis_gaps
oracle_entry: bridge_axis_gaps
registered_operation_keys:
  - mask.bridge_axis_gaps_strict
---

# MASK-16A: Bridge bounded horizontal and vertical barrier gaps

Inherits the [family contract](MASK-16_close_gap_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.bridge_axis_gaps_strict`.

barrier: Binary[H,W] -> temporary: Binary[H,W]

Static parameters: maximum_gap Int64>=0.

## Mathematics

Support: cross-shaped axis support with reach maximum_gap+1, not square; max_gap0 only Q.

## Acceptance

barrier=[1,0,1,0,0,1], maximum_gap1 -> [1,1,1,0,0,1].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
