---
spec_schema_version: 1
id: MASK-04B
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-04
function: lab76_range
oracle_entry: lab76_range
registered_operation_keys:
  - mask.lab76_range_strict
---

# MASK-04B: Normalized-Lab DeltaE76 selector

Inherits the [family contract](MASK-04_color_range_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.lab76_range_strict`.

image: Field[C,H,W], target: Float64[3] -> values: Coverage[H,W]

Static parameters: channels selects l,a*,b*; optional input_description; inner,outer in DeltaE76; curve.

## Mathematics

Use the Lab76 formula with exact factor 100 on the lightness difference; white/description must match.

Support: selected Lab triple; target all 3 controls.

## Acceptance

l difference 1/128, a/b equal gives exact DeltaE76=25/32; inner=0,outer=25/16,linear -> .5.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
