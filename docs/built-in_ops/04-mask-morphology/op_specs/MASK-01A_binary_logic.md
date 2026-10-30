---
spec_schema_version: 1
id: MASK-01A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-01
function: binary_logic
oracle_entry: binary_logic
registered_operation_keys:
  - mask.binary_logic_strict
---

# MASK-01A: Binary Boolean operations

Inherits the [family contract](MASK-01_boolean_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.binary_logic_strict`.

a,b: Binary[H,W] -> values: Binary[H,W], same dtype

Static parameters: operation: String enum and|or|xor|subtract; authoring and.

## Mathematics

Use row A of the family truth table; neither nonzero truthiness nor .5 threshold is allowed.

Support: a[Q] and b[Q], exact.

## Acceptance

a=[0,1,1], b=[1,0,1]: and=[0,0,1], or=[1,1,1], xor=[1,1,0], subtract=[0,1,0].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
