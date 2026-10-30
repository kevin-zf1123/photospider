---
spec_schema_version: 1
id: MASK-03B
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-03
function: range_mask
oracle_entry: range_mask
registered_operation_keys:
  - mask.range_mask_strict
---

# MASK-03B: Hard interval mask

Inherits the [family contract](MASK-03_threshold_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.range_mask_strict`.

input: Field[H,W] -> values: floating Binary[H,W]

Static parameters: lower,upper: finite Float64 lower<=upper; lower_closed,upper_closed: Bool; authoring both true.

## Mathematics

Compare both endpoints, AND the Boolean results.

Support: input[Q].

## Acceptance

[0,.5,1], range [0,1) -> [1,1,0]; empty (1,1] selects nothing.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
