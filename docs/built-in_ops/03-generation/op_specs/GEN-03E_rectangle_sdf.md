---
spec_schema_version: 1
id: GEN-03E
parent_id: GEN-03
function: rectangle_sdf
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.rectangle_sdf_v1_strict
  - generation.rectangle_sdf_v1_accelerated_apple_silicon
  - generation.rectangle_sdf_v1_accelerated_x86_64
---

# GEN-03E: rectangle sdf

Inherits [GEN-03](GEN-03_basic_shapes_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.rectangle_sdf_v1_strict`, `generation.rectangle_sdf_v1_accelerated_apple_silicon`, `generation.rectangle_sdf_v1_accelerated_x86_64`.

center[2], positive half_size[2]; distance[H,W] in pixels.

### Parameters and domain

canvas, dtype default Float64; positive half-sizes are unchecked caller premises.

## Mathematics

q=abs(p-center)-half_size; RN_T(norm2(max(q,0))+min(max(qx,qy),0)), where norm2 denotes Euclidean length. Interior negative, boundary +0.

## Behavior

Support class: Regional.

## Acceptance

Unit half-size gives center -1 and distance sqrt(2) at (2,2).
