---
spec_schema_version: 1
id: NOI-05B
parent_id: NOI-05
function: nearest_point_distances
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.nearest_point_distances_v1_strict
  - noise.nearest_point_distances_v1_accelerated_apple_silicon
  - noise.nearest_point_distances_v1_accelerated_x86_64
---

# NOI-05B: nearest point distances

Inherits [NOI-05](NOI-05_cellular_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.nearest_point_distances_v1_strict`, `noise.nearest_point_distances_v1_accelerated_apple_silicon`, `noise.nearest_point_distances_v1_accelerated_x86_64`.

coordinates[S...,2], [DynamicPoints v1](GEN-08_point_distributions_contract.md); f1,f2 and nearest_id.

### Parameters and domain

dtype; at least two distinct IDs; coincident positions allowed; valid PointSet association.

Inputs: Float32/Float64 coordinate tuples and the canonical `photospider.ops.dynamic_points` v1 schema. Outputs: independent `f1`, `f2` and `nearest_id`. Parameter: String `dtype` (default `float64`). Square roots keep the final RN32/RN64 rounding in every profile.

## Mathematics

Treat published positions as exact points. Sort by (squared distance,id), return RN_T square roots of first two distances and first ID. Coincident distinct IDs can give F1=F2=0; rows do not decide ties.

## Behavior

Support class: Regional. Each query depends on the complete positions, IDs and count of the point set; point payloads and their association are not scanned for validity.

## Acceptance

Bisector tie, coincident sites, row permutation invariance, at least two valid points with distinct IDs as an unchecked premise.
