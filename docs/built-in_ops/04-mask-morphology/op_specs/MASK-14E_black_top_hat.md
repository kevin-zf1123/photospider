---
spec_schema_version: 1
id: MASK-14E
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-14
function: black_top_hat
oracle_entry: black_top_hat
registered_operation_keys:
  - mask.black_top_hat_strict
---

# MASK-14E: Black top hat

Inherits the [family contract](MASK-14_residuals_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.black_top_hat_strict`.

input: Coverage or Binary[H,W] -> values: same dtype/shape

Static parameters: MASK-05 footprint schema.

## Mathematics

RN_T(C(I)-I) using exact family stage semantics; result Coverage/Binary, not signed Field.

Support: (Q+B+B) intersect D, with proper extended intermediate.

## Acceptance

3x3 ring with center0 -> residual center1; full1x1 ->0, never -1.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
