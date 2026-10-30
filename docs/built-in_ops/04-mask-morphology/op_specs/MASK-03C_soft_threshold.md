---
spec_schema_version: 1
id: MASK-03C
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-03
function: soft_threshold
oracle_entry: soft_threshold
registered_operation_keys:
  - mask.soft_threshold_strict
---

# MASK-03C: Soft centered threshold

Inherits the [family contract](MASK-03_threshold_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.soft_threshold_strict`.

input: Field[H,W] -> values: Coverage[H,W]

Static parameters: threshold finite Float64; width finite Float64>=0; curve linear|smoothstep; authoring .5,0,smoothstep.

## Mathematics

Evaluate C from the family and round once; width=0 is hard ge.

Support: input[Q].

## Acceptance

threshold=.5,width=1: x=[0,.5,1] -> [0,.5,1] for both curves.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
