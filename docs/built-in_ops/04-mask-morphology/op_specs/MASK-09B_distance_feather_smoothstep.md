---
spec_schema_version: 1
id: MASK-09B
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-09
function: distance_feather_smoothstep
oracle_entry: distance_feather_smoothstep
registered_operation_keys:
  - mask.distance_feather_smoothstep_strict
---

# MASK-09B: Smoothstep signed-distance feather

Inherits the [family contract](MASK-09_distance_feather_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.distance_feather_smoothstep_strict`.

input: Distance[H,W] -> values: Coverage, same dtype/shape

Static parameters: inner,outer>=0 finite Float64; distance_unit,distance_basis, as family.

## Mathematics

Use exact family width branches and RN_T(1-u*u*(3-2*u)); both-zero is hard d<=0.

Support: input[Q].

Distance inputs admit signed no-feature infinities and exclude NaN as a caller premise. Finite shift preserves infinities; finite threshold and feather map -Inf to 1 and +Inf to 0, as applicable.

## Acceptance

inner=outer=1, d=[-1,0,1] -> [1,.5,0]; inner=outer=0, d=0 ->1.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
