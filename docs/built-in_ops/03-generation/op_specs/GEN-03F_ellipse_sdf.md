---
spec_schema_version: 1
id: GEN-03F
parent_id: GEN-03
function: ellipse_sdf
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.ellipse_sdf_v1_strict
  - generation.ellipse_sdf_v1_accelerated_apple_silicon
  - generation.ellipse_sdf_v1_accelerated_x86_64
---

# GEN-03F: ellipse sdf

Inherits [GEN-03](GEN-03_basic_shapes_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.ellipse_sdf_v1_strict`, `generation.ellipse_sdf_v1_accelerated_apple_silicon`, `generation.ellipse_sdf_v1_accelerated_x86_64`.

center[2], positive radii[2]; distance[H,W] in pixels.

### Parameters and domain

canvas, dtype default Float64, Int64 max_refinements default 4096.

`max_refinements` is the full positive Int64 refinement limit; it is not reduced modulo 2^32. Limits 4096, 2^32, 2^32+1 and INT64_MAX produce identical bits whenever the same rounding certificate is reached within 4096 refinements.

## Mathematics

Signed minimum Euclidean distance to c+(rx*cos(theta),ry*sin(theta)); implicit ellipse test determines sign. Boundary +0; center=-min(rx,ry); circle reduces to norm(p-c)-r. Isolate all relevant minima.

## Behavior

Support class: Regional.

## Acceptance

Ellipse (2,1) has center -1 and distance 1 at (3,0); interior axis points can have off-axis closest points.
