---
spec_schema_version: 1
id: NOI-06A
parent_id: NOI-06
function: fbm_sum
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.fbm_sum_v1_strict
  - noise.fbm_sum_v1_accelerated_apple_silicon
  - noise.fbm_sum_v1_accelerated_x86_64
---

# NOI-06A: fbm sum

Inherits [NOI-06](NOI-06_fractal_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.fbm_sum_v1_strict`, `noise.fbm_sum_v1_accelerated_apple_silicon`, `noise.fbm_sum_v1_accelerated_x86_64`.

coordinates[S...,D], D=2 or3 according to basis; values[S...].

### Parameters and domain

basis=gradient2d_philox/perlin2002_3d; octaves=1..32,frequency>0,lacunarity>1,gain in [0,1]; normalization=none/amplitude_sum default latter; basis RNG/period rules.

## Mathematics

a_i=gain^i, q_i=frequency*lacunarity^i*q exactly; n_i is the unrounded mathematical basis value. Return RN_T(sum a_i*n_i / den), den=1 or sum a_i. Skip proven zero-amplitude terms without inventing out-of-range coordinates. Octaves share the scaled base field, not an independence claim.

## Behavior

Support class: Regional.

## Acceptance

One octave equals basis; gain0 eliminates other octaves; no per-octave four-ULP allocation.
