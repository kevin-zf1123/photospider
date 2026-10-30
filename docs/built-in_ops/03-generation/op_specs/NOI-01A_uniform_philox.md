---
spec_schema_version: 1
id: NOI-01A
parent_id: NOI-01
function: uniform_philox
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.uniform_philox_v1_strict
---

# NOI-01A: uniform philox

Inherits [NOI-01](NOI-01_uniform_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.uniform_philox_v1_strict`.

No Tensor input; raw values[H,W,C] Float32/64.

### Parameters and domain

canvas,C=1..65536,dtype default Float64,seed/stream/frame; signed32 global coordinates; domain 1, draw fixed to 0 without a user-adjustable draw parameter.

## Mathematics

Use Philox4x64-10 and the separately frozen address/bit mapping. Float32 returns halfopen24 j/2^24; Float64 halfopen53 j/2^53, both exactly representable. Native Float32 is not a cast of the Float64 sequence.

## Behavior

Support class: Regional.

## Acceptance

Official 4x64 core KATs, address bounds, negative origin, ROI/order invariance; finite-grid statistics do not certify a runtime sequence.
