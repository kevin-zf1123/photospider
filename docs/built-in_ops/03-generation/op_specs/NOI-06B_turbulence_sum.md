---
spec_schema_version: 1
id: NOI-06B
parent_id: NOI-06
function: turbulence_sum
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.turbulence_sum_v1_strict
  - noise.turbulence_sum_v1_accelerated_apple_silicon
  - noise.turbulence_sum_v1_accelerated_x86_64
---

# NOI-06B: turbulence sum

Inherits [NOI-06](NOI-06_fractal_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.turbulence_sum_v1_strict`, `noise.turbulence_sum_v1_accelerated_apple_silicon`, `noise.turbulence_sum_v1_accelerated_x86_64`.

Same ports as NOI-06A.

### Parameters and domain

Same parameters and unchecked premises as NOI-06A.

## Mathematics

Use NOI-06A's exact octave construction with term_i=abs(n_i), one final RN. Preserve shared-scale and zero-amplitude rules.

## Behavior

Support class: Regional.

## Acceptance

Nonnegative output, integer origin zero, sign-invariant term.
