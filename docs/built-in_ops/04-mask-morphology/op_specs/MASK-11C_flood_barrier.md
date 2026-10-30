---
spec_schema_version: 1
id: MASK-11C
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-11
function: flood_barrier
oracle_entry: flood_barrier
registered_operation_keys:
  - mask.flood_barrier_strict
---

# MASK-11C: Flood regions bounded by a binary barrier

Inherits the [family contract](MASK-11_flood_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.flood_barrier_strict`.

seeds: Binary[H,W], barrier: Binary[H,W] -> values: Binary[H,W]

Static parameters: connectivity: 4|8, authoring 4.

## Mathematics

Reachability from all seeds through barrier=0; callers guarantee that seeds are not blocked.

complete-input seeds/barrier.

The output Binary mask uses the seeds floating dtype.

## Acceptance

barrier=[0,1,0], seed=[1,0,0] -> [1,0,0]; empty seeds -> zeros.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
