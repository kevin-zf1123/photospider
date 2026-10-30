---
spec_schema_version: 1
id: GEN-05C
parent_id: GEN-05
function: angular_gradient_coordinate
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.angular_gradient_coordinate_v1_strict
  - generation.angular_gradient_coordinate_v1_accelerated_apple_silicon
  - generation.angular_gradient_coordinate_v1_accelerated_x86_64
---

# GEN-05C: angular gradient coordinate

Inherits [GEN-05](GEN-05_gradient_coordinates_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.angular_gradient_coordinate_v1_strict`, `generation.angular_gradient_coordinate_v1_accelerated_apple_silicon`, `generation.angular_gradient_coordinate_v1_accelerated_x86_64`.

center[2], offset_turns[1]; t[H,W].

### Parameters and domain

canvas/dtype; finite offset in turns; center_policy=zero.

## Mathematics

At center output +0. Otherwise q=atan2(dy,dx)/(2*pi)-offset_turns, then RN_T(q-floor(q)). Exact seam maps to +0; an interior value rounding to 1 remains 1. No nextDown or second wrap.

## Behavior

Support class: Regional.

## Acceptance

Cardinal directions, offset seam, signed axes and values rounding to the upper endpoint.
