---
spec_schema_version: 1
id: MASK-07C
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-07
function: threshold_distance_field
oracle_entry: threshold_distance_field
registered_operation_keys:
  - mask.threshold_distance_field_strict
---

# MASK-07C: Threshold a signed distance/level set

Inherits the [family contract](MASK-07_offset_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.threshold_distance_field_strict`.

input: Distance[H,W] -> values: floating Binary[H,W]

Static parameters: radius signed finite Float64; distance_unit and distance_basis as B.

## Mathematics

Exact comparison d<=radius; output 1 or +0.

Support: input[Q].

Distance inputs admit signed no-feature infinities and exclude NaN as a caller premise. Finite shift preserves infinities; finite threshold and feather map -Inf to 1 and +Inf to 0, as applicable.

## Acceptance

d=[-.5,0,.5], radius=0 -> [1,1,0].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
