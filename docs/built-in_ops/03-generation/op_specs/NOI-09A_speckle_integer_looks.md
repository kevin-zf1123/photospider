---
spec_schema_version: 1
id: NOI-09A
parent_id: NOI-09
function: speckle_integer_looks
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.speckle_integer_looks_v1_strict
  - noise.speckle_integer_looks_v1_accelerated_apple_silicon
  - noise.speckle_integer_looks_v1_accelerated_x86_64
---

# NOI-09A: speckle integer looks

Inherits [NOI-09](NOI-09_grain_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.speckle_integer_looks_v1_strict`, `noise.speckle_integer_looks_v1_accelerated_apple_silicon`, `noise.speckle_integer_looks_v1_accelerated_x86_64`.

Finite raw signal[H,W,C]; same-shape Float32/64 values.

### Parameters and domain

Integer looks L=1..64; seed/stream/frame,domain9,draw j=0..L-1.

Input: Float32/Float64 HWC `signal`; output `values` in the requested dtype. Parameters: Int64 `looks`, `seed`, `stream` (required); Int64 `frame` and origins (default 0); String `dtype` (default `float64`); Int64 `max_refinements` (default 4096); optional `height`/`width` must match the input schema; C<=65536.

## Mathematics

Each look j takes open52 from word 0 of its domain 9 block with draw=j; words 1/2/3 are unused. G=-sum_j ln(open52_j)/L; output RN_T(signal*G) with no intermediate RN of G. Ideal continuous multiplier has mean1,variance1/L; disclose finite-grid bias. Signed raw signals allowed; physical intensity interpretation requires nonnegative values. Signed-zero signal follows multiplication sign.

## Behavior

Support class: Regional.

## Acceptance

Zero signal, L1 exponential case, positive output for positive signal; larger L does not imply pixelwise smaller deviations.
