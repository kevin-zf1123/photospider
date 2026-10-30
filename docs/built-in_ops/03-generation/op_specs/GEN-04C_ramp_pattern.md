---
spec_schema_version: 1
id: GEN-04C
parent_id: GEN-04
function: ramp_pattern
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.ramp_pattern_v1_strict
  - generation.ramp_pattern_v1_accelerated_apple_silicon
  - generation.ramp_pattern_v1_accelerated_x86_64
---

# GEN-04C: ramp pattern

Inherits [GEN-04](GEN-04_test_patterns_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.ramp_pattern_v1_strict`, `generation.ramp_pattern_v1_accelerated_apple_silicon`, `generation.ramp_pattern_v1_accelerated_x86_64`.

No Tensor inputs; values[H,W].

### Parameters and domain

canvas, axis=x/y, dtype default Float64; finite lo/hi default 0/1.

## Mathematics

For axis length N>1 return RN_T(((N-1-i)*lo+i*hi)/(N-1)), copying selected endpoints. N=1 returns lo. This differs from edge-normalized pixel centers.

## Behavior

Support class: Regional.

## Acceptance

N=3 gives 0,0.5,1; N=1 gives lo; descending endpoints allowed.
