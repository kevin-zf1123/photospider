---
spec_schema_version: 1
id: GEN-04D
parent_id: GEN-04
function: impulse_pattern
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.impulse_pattern_v1_strict
---

# GEN-04D: impulse pattern

Inherits [GEN-04](GEN-04_test_patterns_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.impulse_pattern_v1_strict`.

index[2] Int64 in local xy order; values[H,W].

### Parameters and domain

canvas, dtype default Float64; finite amplitude/background default 1/0; index must be in bounds.

## Mathematics

Select amplitude exactly at index, background elsewhere. This is a discrete pixel impulse, not a continuous Dirac distribution or subpixel splat.

## Behavior

Support class: Regional.

The index is in bounds as a caller premise, including zero amplitude.

## Acceptance

3x3 center impulse; valid boundary indices; zero amplitude.
