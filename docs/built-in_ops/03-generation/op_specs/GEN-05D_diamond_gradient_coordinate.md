---
spec_schema_version: 1
id: GEN-05D
parent_id: GEN-05
function: diamond_gradient_coordinate
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.diamond_gradient_coordinate_v1_strict
  - generation.diamond_gradient_coordinate_v1_accelerated_apple_silicon
  - generation.diamond_gradient_coordinate_v1_accelerated_x86_64
---

# GEN-05D: diamond gradient coordinate

Inherits [GEN-05](GEN-05_gradient_coordinates_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.diamond_gradient_coordinate_v1_strict`, `generation.diamond_gradient_coordinate_v1_accelerated_apple_silicon`, `generation.diamond_gradient_coordinate_v1_accelerated_x86_64`.

center[2], positive radii[2]; t[H,W].

### Parameters and domain

canvas/dtype.

## Mathematics

RN_T(abs(px-cx)/rx+abs(py-cy)/ry); radii are axis intercepts, not Euclidean distances.

## Behavior

Support class: Regional.

## Acceptance

p-c=(1,1), radii=(2,2) gives 1; center zero.
