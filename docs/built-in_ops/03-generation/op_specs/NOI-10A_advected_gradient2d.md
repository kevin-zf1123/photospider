---
spec_schema_version: 1
id: NOI-10A
parent_id: NOI-10
function: advected_gradient2d
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.advected_gradient2d_v1_strict
  - noise.advected_gradient2d_v1_accelerated_apple_silicon
  - noise.advected_gradient2d_v1_accelerated_x86_64
---

# NOI-10A: advected gradient2d

Inherits [NOI-10](NOI-10_temporal_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.advected_gradient2d_v1_strict`, `noise.advected_gradient2d_v1_accelerated_apple_silicon`, `noise.advected_gradient2d_v1_accelerated_x86_64`.

coordinates[S...,2],velocity[2],time[1]; values[S...].

### Parameters and domain

dtype and NOI-04B seed/stream/period; base frame fixed0; velocity coordinate units/second, time seconds.

Inputs are Float32/Float64 coordinates and controls. Parameters: String `dtype` (default `float64`); Int64 `seed`, `stream` (required); Int64 `period_x`, `period_y` (default 0, no declared period). The basis frame is fixed at 0 and there is no `frame` parameter.

## Mathematics

Evaluate the unrounded mathematical NOI-04B field at q=coordinates-velocity*time, then one final RN. No intermediate coordinate RN; positive velocity moves features in positive direction.

## Behavior

Support class: Regional. All demanded advected corners fit signed32 as a caller premise; no wrapping sequence is defined outside that domain.

## Acceptance

Zero velocity/time identities, sign of advection, cell continuity; no perceptual antialias guarantee.
