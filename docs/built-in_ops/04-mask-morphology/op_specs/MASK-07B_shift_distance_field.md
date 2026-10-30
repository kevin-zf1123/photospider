---
spec_schema_version: 1
id: MASK-07B
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-07
function: shift_distance_field
oracle_entry: shift_distance_field
registered_operation_keys:
  - mask.shift_distance_field_strict
---

# MASK-07B: Shift a signed level set

Inherits the [family contract](MASK-07_offset_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.shift_distance_field_strict`.

input: Distance[H,W] -> values: Distance[H,W], level_set basis

Static parameters: radius signed finite Float64; distance_unit and distance_basis explicitly supplied/consistent with input.

## Mathematics

RN_T(d-radius); radius ±0 exact copy. Output is level_set, not newly certified SDF.

Support: input[Q].

Distance inputs admit signed no-feature infinities and exclude NaN as a caller premise. Finite shift preserves infinities; finite threshold and feather map -Inf to 1 and +Inf to 0, as applicable.

## Acceptance

d=[-1,0,1], radius=.25 -> [-1.25,-.25,.75].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
