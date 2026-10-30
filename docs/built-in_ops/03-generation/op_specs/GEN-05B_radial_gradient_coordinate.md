---
spec_schema_version: 1
id: GEN-05B
parent_id: GEN-05
function: radial_gradient_coordinate
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.radial_gradient_coordinate_v1_strict
  - generation.radial_gradient_coordinate_v1_accelerated_apple_silicon
  - generation.radial_gradient_coordinate_v1_accelerated_x86_64
---

# GEN-05B: radial gradient coordinate

Inherits [GEN-05](GEN-05_gradient_coordinates_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.radial_gradient_coordinate_v1_strict`, `generation.radial_gradient_coordinate_v1_accelerated_apple_silicon`, `generation.radial_gradient_coordinate_v1_accelerated_x86_64`.

center[2], positive radii[2]; t[H,W].

### Parameters and domain

canvas/dtype; radii in pixels; no rotation.

## Mathematics

RN_T(sqrt(((px-cx)/rx)^2+((py-cy)/ry)^2)). Elliptic level sets do not make this Euclidean signed distance.

## Behavior

Support class: Regional.

## Acceptance

Center zero, unit-radius 3-4-5 point gives 5, unequal radii.
