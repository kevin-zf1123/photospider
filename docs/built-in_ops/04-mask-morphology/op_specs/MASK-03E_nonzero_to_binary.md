---
spec_schema_version: 1
id: MASK-03E
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-03
function: nonzero_to_binary
oracle_entry: nonzero_to_binary
registered_operation_keys:
  - mask.nonzero_to_binary_strict
---

# MASK-03E: Explicit UInt8 truthiness adapter

Inherits the [family contract](MASK-03_threshold_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.nonzero_to_binary_strict`.

input: UInt8[H,W], raw bytes -> values: Float32/Float64 Binary[H,W]

Static parameters: output_dtype: float32|float64.

## Mathematics

Return 0 iff input byte is 0, otherwise 1.

Support: input[Q], no color or coverage validation.

## Acceptance

[0,1,128,255] -> [0,1,1,1].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
