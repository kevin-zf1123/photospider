---
spec_schema_version: 1
id: PTH-11A
parent_id: PTH-11
function: centerline_distance
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.centerline_distance_v1_strict
  - path.centerline_distance_v1_accelerated_apple_silicon
  - path.centerline_distance_v1_accelerated_x86_64
---

# PTH-11A: centerline distance

Inherits [PTH-11](PTH-11_distance_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.centerline_distance_v1_strict`, `path.centerline_distance_v1_accelerated_apple_silicon`, `path.centerline_distance_v1_accelerated_x86_64`.

Core/Bezier PathSet; distance[H,W],closest[H,W,2] Float64,segment_id Int64,local_t Float64.

### Parameters and domain

canvas/dtype; optional positive finite max_distance clips distance only; closest still requires true argmin.

## Mathematics

Minimize Euclidean distance over all segments/t. Line projection clamps exact dot ratio; zero-length t0. Bezier candidates include endpoints and every real stationary root in[0,1] (quadratic degree3,cubic degree5). Compare exact distances/ties. Round distance, closest point and t independently. M-only is a point; empty only permits distance-only positive truncation, otherwise NoSolution.

## Behavior

Support class: Regional.

Each pixel depends on the complete path. Equal-distance ties are never discarded by any culling.

## Acceptance

3-4-5, on-curve zero, U-curve multiple minima, lexicographic tie, empty truncation and rounded-t reconstruction difference.
