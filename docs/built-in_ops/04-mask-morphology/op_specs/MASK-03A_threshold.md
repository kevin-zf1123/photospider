---
spec_schema_version: 1
id: MASK-03A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-03
function: threshold
oracle_entry: threshold
registered_operation_keys:
  - mask.threshold_strict
---

# MASK-03A: Hard scalar threshold

Inherits the [family contract](MASK-03_threshold_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.threshold_strict`.

input: Field[H,W] -> values: floating Binary[H,W]

Static parameters: threshold: finite Float64 (authoring .5); comparison: ge|gt|le|lt (authoring ge).

## Mathematics

Return exact typed 1 when the exact comparison holds, otherwise +0.

Support: input[Q].

## Acceptance

x=[.25,.5,.75], threshold=.5, ge -> [0,1,1]; gt -> [0,0,1].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
