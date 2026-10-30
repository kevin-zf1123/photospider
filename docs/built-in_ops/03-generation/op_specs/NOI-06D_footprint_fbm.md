---
spec_schema_version: 1
id: NOI-06D
parent_id: NOI-06
function: footprint_fbm
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.footprint_fbm_v1_strict
  - noise.footprint_fbm_v1_accelerated_apple_silicon
  - noise.footprint_fbm_v1_accelerated_x86_64
---

# NOI-06D: footprint fbm

Inherits [NOI-06](NOI-06_fractal_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.footprint_fbm_v1_strict`, `noise.footprint_fbm_v1_accelerated_apple_silicon`, `noise.footprint_fbm_v1_accelerated_x86_64`.

coordinates[S...,D], footprint[S...] finite nonnegative pre-frequency coordinate radius; values[S...].

### Parameters and domain

NOI-06A parameters; fbm terms only; explicit upstream footprint, never inferred from ROI size.

## Mathematics

Inherit NOI-06A's exact basis coordinates, full weighted sum and one final RN, then multiply a_i by clamp(2-2*footprint*frequency*lacunarity^i,0,1). amplitude_sum denominator is the unattenuated sum gain^i, avoiding cutoff division by zero or amplification. Proven zero weights may skip basis evaluation.

## Behavior

Support class: Regional. This is an antialiasing heuristic, not strict band limitation. Demanded coordinates, footprint and shared controls are assumed to satisfy their domains.

## Acceptance

footprint0 matches A; footprint*frequency>=1 gives +0; continuous cutoff; spectral quality distinct from numeric accuracy.
