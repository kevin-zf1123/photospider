---
spec_schema_version: 1
id: MASK-14B
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-14
function: inner_border
oracle_entry: inner_border
registered_operation_keys:
  - mask.inner_border_strict
---

# MASK-14B: Inner morphological border

Inherits the [family contract](MASK-14_residuals_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.inner_border_strict`.

input: Coverage or Binary[H,W] -> values: same dtype/shape

Static parameters: MASK-05 footprint schema.

## Mathematics

RN_T(I-E(I)) using exact family stage semantics; result Coverage/Binary, not signed Field.

Support: (Q+B) intersect D, with proper extended intermediate.

## Acceptance

3x3 full ones, square r1 -> border ones with center0.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
