---
spec_schema_version: 1
id: MASK-06B
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-06
function: closing
oracle_entry: closing
registered_operation_keys:
  - mask.closing_strict
---

# MASK-06B: Close a soft or binary mask

Inherits the [family contract](MASK-06_open_close_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.closing_strict`.

input: Coverage or Binary[H,W] -> values: same dtype/shape

Static parameters: MASK-05 footprint schema.

## Mathematics

E(D(input)) without intermediate crop; no per-stage zero reset.

Support: Q+B+B intersect D.

## Acceptance

full 1x1 input, square r=1 -> 1, not 0; padded [1,0,1] line closes the gap when embedded away from borders.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
