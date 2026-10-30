---
spec_schema_version: 1
id: GEN-05A
parent_id: GEN-05
function: linear_gradient_coordinate
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.linear_gradient_coordinate_v1_strict
  - generation.linear_gradient_coordinate_v1_accelerated_apple_silicon
  - generation.linear_gradient_coordinate_v1_accelerated_x86_64
---

# GEN-05A: linear gradient coordinate

Inherits [GEN-05](GEN-05_gradient_coordinates_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.linear_gradient_coordinate_v1_strict`, `generation.linear_gradient_coordinate_v1_accelerated_apple_silicon`, `generation.linear_gradient_coordinate_v1_accelerated_x86_64`.

start[2], end[2]; t[H,W].

### Parameters and domain

canvas/dtype; distinct finite endpoints.

## Mathematics

RN_T(dot(p-start,end-start)/dot(end-start,end-start)); no [0,1] clipping. Evaluate the full rational expression before rounding.

## Behavior

Support class: Regional.

Distinct finite endpoints are an unchecked caller premise.

## Acceptance

Endpoints, midpoint, extrapolation and distinct endpoint geometry.
