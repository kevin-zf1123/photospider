---
spec_schema_version: 1
id: MASK-02A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-02
function: invert
oracle_entry: invert
registered_operation_keys:
  - mask.invert_strict
---

# MASK-02A: Invert coverage on the current canvas

Inherits the [family contract](MASK-02_invert_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.invert_strict`.

input: Coverage or Binary[H,W] -> values: same dtype/shape

Static parameters: None..

## Mathematics

values=RN_T(1-input). Generated zero is +0.

Support: input[Q] is Data.

## Acceptance

[0,.25,1] -> [1,.75,0]; Float64 2^-60 -> 1 -> 0 after two inversions.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
