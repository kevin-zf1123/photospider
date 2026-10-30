---
spec_schema_version: 1
id: MASK-06A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-06
function: opening
oracle_entry: opening
registered_operation_keys:
  - mask.opening_strict
---

# MASK-06A: Open a soft or binary mask

Inherits the [family contract](MASK-06_open_close_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.opening_strict`.

input: Coverage or Binary[H,W] -> values: same dtype/shape

Static parameters: MASK-05 footprint schema.

## Mathematics

D(E(input)) on the infinite zero extension; stage extrema copy exact bits.

Support: Q+B+B intersect D. Exact intermediate extension as family.

## Acceptance

single 1 pixel, square r=1 -> 0; r=0 is exact identity.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
