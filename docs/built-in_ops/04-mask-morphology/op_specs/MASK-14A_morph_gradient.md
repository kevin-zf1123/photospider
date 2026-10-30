---
spec_schema_version: 1
id: MASK-14A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-14
function: morph_gradient
oracle_entry: morph_gradient
registered_operation_keys:
  - mask.morph_gradient_strict
---

# MASK-14A: Morphological gradient

Inherits the [family contract](MASK-14_residuals_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.morph_gradient_strict`.

input: Coverage or Binary[H,W] -> values: same dtype/shape

Static parameters: MASK-05 footprint schema.

## Mathematics

RN_T(D(I)-E(I)) using exact family stage semantics; result Coverage/Binary, not signed Field.

Support: (Q+B) intersect D, with proper extended intermediate.

## Acceptance

single1 in a 3x3 zero image, square r1 -> gradient all ones.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
