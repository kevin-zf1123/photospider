---
spec_schema_version: 1
id: MASK-04A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-04
function: coordinate_range
oracle_entry: coordinate_range
registered_operation_keys:
  - mask.coordinate_range_strict
---

# MASK-04A: Scaled coordinate-distance mask

Inherits the [family contract](MASK-04_color_range_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.coordinate_range_strict`.

image: Field[C,H,W], target: Float64[K], scales: Float64[K] -> values: Coverage[H,W]

Static parameters: channels; optional input_description; inner,outer; curve, as family.

## Mathematics

Use scaled Euclidean distance and the exact family radial ramp.

Support: image selected components; no complete-group payload scan. complete-input target/scales control arrays for any nonempty Q.

## Acceptance

single coordinate image=[0,.5,1], target=[0], scales=[1], inner=0,outer=1,linear -> [1,.5,0].

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
