---
spec_schema_version: 1
id: MASK-01C
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-01
function: independent_coverage
oracle_entry: independent_coverage
registered_operation_keys:
  - mask.independent_coverage_strict
---

# MASK-01C: Independent-coverage operations

Inherits the [family contract](MASK-01_boolean_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.independent_coverage_strict`.

a,b: Coverage[H,W] -> values: Coverage, same shape/dtype

Static parameters: operation: String enum and|or|xor|subtract; authoring and.

## Mathematics

Use row C as one exact polynomial then RN_T; no intermediate a+b rounding.

Support: both masks at Q.

## Acceptance

a=b=.5: and=.25, or=.75, xor=.5, subtract=.25.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
