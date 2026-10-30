---
spec_schema_version: 1
id: GEN-04E
parent_id: GEN-04
function: zone_plate
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.zone_plate_v1_strict
  - generation.zone_plate_v1_accelerated_apple_silicon
  - generation.zone_plate_v1_accelerated_x86_64
---

# GEN-04E: zone plate

Inherits [GEN-04](GEN-04_test_patterns_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.zone_plate_v1_strict`, `generation.zone_plate_v1_accelerated_apple_silicon`, `generation.zone_plate_v1_accelerated_x86_64`.

center[2], k[1], phase[1]; values[H,W].

### Parameters and domain

canvas/dtype; finite k in cycles/px^2, phase radians; finite lo/hi default 0/1.

## Mathematics

RN_T(lo+(hi-lo)*(1+cos(2*pi*k*norm(p-center)^2+phase))/2). No implicit Nyquist cutoff; aliasing is part of the diagnostic pattern.

## Behavior

Support class: Regional.

## Acceptance

k=0, phase=0 gives hi; exact mathematical pi phase gives lo, without treating rounded input pi as exact pi.
