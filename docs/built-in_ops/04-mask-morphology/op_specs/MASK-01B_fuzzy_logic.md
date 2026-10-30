---
spec_schema_version: 1
id: MASK-01B
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-01
function: fuzzy_logic
oracle_entry: fuzzy_logic
registered_operation_keys:
  - mask.fuzzy_logic_strict
---

# MASK-01B: Fuzzy mask operations

Inherits the [family contract](MASK-01_boolean_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.fuzzy_logic_strict`.

a,b: Coverage[H,W] -> values: Coverage, same shape/dtype

Static parameters: operation: String enum and|or|xor|subtract; authoring and.

## Mathematics

Use row B, with exact extrema selection and the family tie rule.

Support: both masks at Q.

## Acceptance

a=b=.5: and=.5, or=.5, xor=0, subtract=.5. Equality is deliberately not ordinary set subtraction.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
