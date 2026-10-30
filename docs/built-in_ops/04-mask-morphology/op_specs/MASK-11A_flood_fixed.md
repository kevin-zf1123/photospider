---
spec_schema_version: 1
id: MASK-11A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-11
function: flood_fixed
oracle_entry: flood_fixed
registered_operation_keys:
  - mask.flood_fixed_strict
---

# MASK-11A: Fixed-seed-range region growing

Inherits the [family contract](MASK-11_flood_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.flood_fixed_strict`.

image: Field[C,H,W], seeds: Binary[H,W], barrier: Binary[H,W] -> values: Binary[H,W]

Static parameters: connectivity: 4|8; tolerance finite Float64>=0; authoring 4,0.

## Mathematics

Union independent seed-conditioned reachable sets, as family.

complete-input image, seeds and barrier; all nonempty Q share full closure.

The output Binary mask uses the seeds floating dtype.

## Acceptance

image line [0,1,2], seed at first, tolerance=1 -> [1,1,0].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
