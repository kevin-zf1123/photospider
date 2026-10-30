---
spec_schema_version: 1
id: NOI-02A
parent_id: NOI-02
function: gaussian_box_muller
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.gaussian_box_muller_v1_strict
  - noise.gaussian_box_muller_v1_accelerated_apple_silicon
  - noise.gaussian_box_muller_v1_accelerated_x86_64
---

# NOI-02A: gaussian box muller

Inherits [NOI-02](NOI-02_gaussian_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.gaussian_box_muller_v1_strict`, `noise.gaussian_box_muller_v1_accelerated_apple_silicon`, `noise.gaussian_box_muller_v1_accelerated_x86_64`.

mean[C], sigma[C] Float32/64; values[H,W,C].

### Parameters and domain

canvas/dtype,seed/stream/frame, domain 2; finite mean, finite sigma>=0,C<=65536.

## Mathematics

sigma=0 returns RN_T(mean) without log. Otherwise open52 u and halfopen53 v define RN_T(mean+sigma*sqrt(-2*ln(u))*cos(2*pi*v)). No intermediate double rounding; one independent cos sample per channel, no neighbor pairing. For each channel, take u from word 0 and v from word 1 of the same domain 2 block, using the shared open52 and halfopen53 mappings. Words 2 and 3 are unused; draw is fixed to 0 without a user-adjustable draw parameter.

## Behavior

Support class: Regional. The mean and sigma controls are assumed valid (unchecked caller premise).

## Acceptance

sigma zero, open endpoints, analytic quarter-turn phases, small sigma/tails. Measured Box-Muller is not a strict golden.
