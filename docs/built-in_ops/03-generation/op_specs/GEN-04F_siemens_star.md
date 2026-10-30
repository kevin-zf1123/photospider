---
spec_schema_version: 1
id: GEN-04F
parent_id: GEN-04
function: siemens_star
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.siemens_star_v1_strict
  - generation.siemens_star_v1_accelerated_apple_silicon
  - generation.siemens_star_v1_accelerated_x86_64
---

# GEN-04F: siemens star

Inherits [GEN-04](GEN-04_test_patterns_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.siemens_star_v1_strict`, `generation.siemens_star_v1_accelerated_apple_silicon`, `generation.siemens_star_v1_accelerated_x86_64`.

center[2], phase[1]; values[H,W].

### Parameters and domain

canvas/dtype; cycles n=1..4096, phase radians; lo/hi default 0/1.

## Mathematics

Away from center use RN_T(lo+(hi-lo)*(1+cos(n*atan2(dy,dx)+phase))/2). Center is RN_T((lo+hi)/2). This is a cosine star, not thresholded spokes.

## Behavior

Support class: Regional.

## Acceptance

n=4, phase=0 gives hi along axes, midpoint at center; no artificial black seam.
