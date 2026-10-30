---
spec_schema_version: 1
id: MASK-05A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-05
function: dilate
oracle_entry: dilate
registered_operation_keys:
  - mask.dilate_strict
---

# MASK-05A: Flat dilation

Inherits the [family contract](MASK-05_morphology_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.dilate_strict`.

input: Coverage or Binary[H,W] -> values: same dtype/shape

Static parameters: footprint and conditional radius/offsets exactly as family.

## Mathematics

Apply max on the zero-extended footprint; copy the winning sample bits with the family tie rule.

Support: (Q+B) intersect D; no implicit bounding gaps. Outside taps are constants.

## Acceptance

input=[0,1,0], square r=1 -> [1,1,1].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
