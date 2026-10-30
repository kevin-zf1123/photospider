---
spec_schema_version: 1
id: NOI-10B
parent_id: NOI-10
function: temporal_perlin3d
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.temporal_perlin3d_v1_strict
  - noise.temporal_perlin3d_v1_accelerated_apple_silicon
  - noise.temporal_perlin3d_v1_accelerated_x86_64
---

# NOI-10B: temporal perlin3d

Inherits [NOI-10](NOI-10_temporal_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.temporal_perlin3d_v1_strict`, `noise.temporal_perlin3d_v1_accelerated_apple_silicon`, `noise.temporal_perlin3d_v1_accelerated_x86_64`.

coordinates[S...,2],time[1],time_scale[1],z_offset[1]; values[S...].

### Parameters and domain

dtype; fixed NOI-04A basis, no seed; finite time, positive time_scale in 1/second.

Inputs are Float32/Float64 coordinates and scalar controls; `values` dtype defaults to `float64`. There are no RNG parameters.

## Mathematics

z=time*time_scale+z_offset exactly; RN_T(Perlin2002(x,y,z)). Time period is256/time_scale; arbitrary time queries need no accumulated frame state. Spatial frequency is explicit upstream mapping.

## Behavior

Support class: Regional. The scalar controls apply to every coordinate.

## Acceptance

Slice equivalence and exact periodic coordinates; no aperiodicity/STBN claim.
