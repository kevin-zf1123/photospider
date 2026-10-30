---
spec_schema_version: 1
id: MASK-07A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-07
function: offset_discrete
oracle_entry: offset_discrete
registered_operation_keys:
  - mask.offset_discrete_strict
---

# MASK-07A: Physical-metric binary center offset

Inherits the [family contract](MASK-07_offset_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.offset_discrete_strict`.

input: Binary[H,W] -> values: same dtype/shape

Static parameters: radius signed finite Float64; sy,sx>0 finite Float64; metric l1|l2|linf; authoring 1,1,1,l2.

## Mathematics

Use metric B_r and dilation for nonnegative radius, erosion for negative radius.

Support: Q+B_r intersect D; exact integer/rational stencil.

## Acceptance

single point, unit spacing: r=.5 unchanged; r=1 cross of five; r=1.5 includes four diagonals. Erosion at -.5 is identity.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
