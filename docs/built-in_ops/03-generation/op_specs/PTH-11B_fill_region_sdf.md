---
spec_schema_version: 1
id: PTH-11B
parent_id: PTH-11
function: fill_region_sdf
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.fill_region_sdf_v1_strict
  - path.fill_region_sdf_v1_accelerated_apple_silicon
  - path.fill_region_sdf_v1_accelerated_x86_64
---

# PTH-11B: fill region sdf

Inherits [PTH-11](PTH-11_distance_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.fill_region_sdf_v1_strict`, `path.fill_region_sdf_v1_accelerated_apple_silicon`, `path.fill_region_sdf_v1_accelerated_x86_64`.

Polyline PathSet; distance[H,W]; only `distance` is a public output of this member; closest/provenance would require a formal associated schema.

### Parameters and domain

canvas/dtype/fill_rule; optional positive finite max_distance; source curves explicitly flattened.

## Mathematics

Use regularized filled region and its exposed boundary. Distance to that boundary with inside negative, boundary +0; remove internal shared/overlap edges. Optional signed magnitude clipping. Empty region returns +max only when supplied, otherwise NoSolution.

## Behavior

Support class: Regional.

Each query depends on the complete exposed boundary.

## Acceptance

Rectangle interior, adjacent union shared edge not zero, holes, repeated evenodd contours empty and signed clipping.
