---
spec_schema_version: 1
id: MASK-15B
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-15
function: thin_distance_ordered
oracle_entry: thin_distance_ordered
registered_operation_keys:
  - mask.thin_distance_ordered_strict
---

# MASK-15B: Distance-priority topology thinning with radius

Inherits the [family contract](MASK-15_topology_reconstruction_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.thin_distance_ordered_strict`.

input: Binary[H,W] -> skeleton: Binary[H,W], radius: Float32/64[H,W]

Static parameters: sy,sx>0 finite; output_dtype float32|float64.

## Mathematics

Same deletion predicate as A, fixed original EDT order; radius from original background distances at survivors.

complete-input input for either output, not fixed halo.

## Acceptance

single1 with unit spacing -> skeleton1 and radius 1; radius is center-to-background, not .5.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
