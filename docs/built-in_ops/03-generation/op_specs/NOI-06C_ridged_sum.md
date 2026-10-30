---
spec_schema_version: 1
id: NOI-06C
parent_id: NOI-06
function: ridged_sum
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.ridged_sum_v1_strict
  - noise.ridged_sum_v1_accelerated_apple_silicon
  - noise.ridged_sum_v1_accelerated_x86_64
---

# NOI-06C: ridged sum

Inherits [NOI-06](NOI-06_fractal_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.ridged_sum_v1_strict`, `noise.ridged_sum_v1_accelerated_apple_silicon`, `noise.ridged_sum_v1_accelerated_x86_64`.

Same ports as NOI-06A.

### Parameters and domain

Same parameters and unchecked premises as NOI-06A.

## Mathematics

Inherit the complete exact octave coordinates, amplitudes, denominator and zero-amplitude elimination of NOI-06A, replacing only term_i with (1-abs(n_i)/2)^2 under the base bound abs(n_i)<=2. No recursive feedback weighting. Complete octave sum has one final RN.

## Behavior

Support class: Regional.

## Acceptance

Amplitude-normalized output in [0,1]; zero base terms give 1; no identity with other ridged-multifractal software.
