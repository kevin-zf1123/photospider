---
spec_schema_version: 1
id: MASK-07D
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D2_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-07
function: offset_polygon_grid
oracle_entry: offset_polygon_grid
registered_operation_keys:
  - mask.offset_polygon_grid_strict
---

# MASK-07D: Continuous polygon offset with fixed sample-grid coverage

Inherits the [family contract](MASK-07_offset_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Registered operation key: `mask.offset_polygon_grid_strict`.

vertices: Field[V,2] -> values: Coverage[H,W]

Static parameters: height,width (positive Int64 output dimensions H,W); sy,sx; radius; sample_pattern; output_dtype; grid_center requires samples_per_axis, vulkan_standard requires sample_count, as family.

## Mathematics

Use exact signed point-to-polygon distance membership at the selected profile's N sample points; final rational count rounding.

complete-input vertices satisfying the polygon premises for any nonempty Q; output pixels independently evaluable.

## Acceptance

polygon cell corners (-.5,-.5),(-.5,.5),(.5,.5),(.5,-.5), H=W=1,n=2: r=0 ->1; r=-.3 ->0.

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
