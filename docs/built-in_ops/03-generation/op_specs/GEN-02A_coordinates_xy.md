---
spec_schema_version: 1
id: GEN-02A
parent_id: GEN-02
function: coordinates_xy
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.coordinates_xy_v1_strict
  - generation.coordinates_xy_v1_accelerated_apple_silicon
  - generation.coordinates_xy_v1_accelerated_x86_64
---

# GEN-02A: coordinates xy

Inherits [GEN-02](GEN-02_coordinate_grid_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.coordinates_xy_v1_strict`, `generation.coordinates_xy_v1_accelerated_apple_silicon`, `generation.coordinates_xy_v1_accelerated_x86_64`.

No Tensor inputs; coordinates[H,W,2] Float32/64, component order xy.

### Parameters and domain

canvas, dtype default Float64; space=pixel/normalized_edge, default pixel.

## Mathematics

pixel returns RN_T(origin_x+x+1/2), RN_T(origin_y+y+1/2). normalized_edge returns RN_T((x+1/2)/W), RN_T((y+1/2)/H). Construct exact rational coordinates before rounding.

## Behavior

Support class: Regional.

## Acceptance

W=4 gives x=(1/8,3/8,5/8,7/8); 1x1 normalized center=(0.5,0.5); origin=(10,-2) gives first center=(10.5,-1.5).
