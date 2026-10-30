---
spec_schema_version: 1
id: MASK-14D
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-14
function: white_top_hat
oracle_entry: white_top_hat
registered_operation_keys:
  - mask.white_top_hat_strict
---

# MASK-14D: White top hat

Inherits the [family contract](MASK-14_residuals_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.white_top_hat_strict`.

input: Coverage or Binary[H,W] -> values: same dtype/shape

Static parameters: MASK-05 footprint schema.

## Mathematics

RN_T(I-O(I)) using exact family stage semantics; result Coverage/Binary, not signed Field.

Support: (Q+B+B) intersect D, with proper extended intermediate.

## Acceptance

isolated center1 removed by opening -> residual center1.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
