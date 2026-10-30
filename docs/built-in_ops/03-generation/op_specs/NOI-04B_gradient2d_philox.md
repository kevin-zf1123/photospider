---
spec_schema_version: 1
id: NOI-04B
parent_id: NOI-04
function: gradient2d_philox
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.gradient2d_philox_v1_strict
  - noise.gradient2d_philox_v1_accelerated_apple_silicon
  - noise.gradient2d_philox_v1_accelerated_x86_64
---

# NOI-04B: gradient2d philox

Inherits [NOI-04](NOI-04_gradient_noise_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.gradient2d_philox_v1_strict`, `noise.gradient2d_philox_v1_accelerated_apple_silicon`, `noise.gradient2d_philox_v1_accelerated_x86_64`.

coordinates[S...,2]; values[S...] Float32/64.

### Parameters and domain

dtype,seed/stream/frame; corner coordinates signed32; period_x/y=0 or 1..2^31-1.

## Mathematics

Exact floor/local fraction; positive periods apply Euclidean modulo to valid corner indices before RNG. Domain6, channel/draw=0; the low three bits of word 0 (word0 & 7) choose (1,0),(-1,0),(0,1),(0,-1),(1,1),(-1,1),(1,-1),(-1,-1). Diagonals are not normalized. Four fade5-weighted dots form one RN expression.

## Behavior

Support class: Regional.

## Acceptance

Integer zeros, C2 seams, explicit periods, period1, address bounds and exact gradient selection. period0 does not promise mathematical aperiodicity.
