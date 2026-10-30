---
spec_schema_version: 1
id: PTH-11C
parent_id: PTH-11
function: stroke_region_sdf
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.stroke_region_sdf_v1_strict
  - path.stroke_region_sdf_v1_accelerated_apple_silicon
  - path.stroke_region_sdf_v1_accelerated_x86_64
---

# PTH-11C: stroke region sdf

Inherits [PTH-11](PTH-11_distance_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.stroke_region_sdf_v1_strict`, `path.stroke_region_sdf_v1_accelerated_apple_silicon`, `path.stroke_region_sdf_v1_accelerated_x86_64`.

Path and explicit width recipe supported by PTH-10A/B; distance[H,W].

### Parameters and domain

canvas/dtype,cap/join/miter or variable-round rules, optional max_distance.

## Mathematics

Distance to the actual exposed boundary of the exact stroke union, negative inside. Isolated round point is disk SDF. Min of component SDFs is not a general exact union distance inside overlaps. Using polygonized outlines requires an explicit PTH-10C->PTH-11B workflow with its geometry error.

## Behavior

Support class: Regional.

Each query depends on the complete stroke boundary.

## Acceptance

Disk center/boundary/outside, butt ends, intersecting bands' true boundary and empty zero-width region.
