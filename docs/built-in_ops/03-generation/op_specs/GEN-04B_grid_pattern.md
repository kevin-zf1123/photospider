---
spec_schema_version: 1
id: GEN-04B
parent_id: GEN-04
function: grid_pattern
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.grid_pattern_v1_strict
---

# GEN-04B: grid pattern

Inherits [GEN-04](GEN-04_test_patterns_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.grid_pattern_v1_strict`.

cell_size[2], offset[2], line_width[2]; values[H,W].

### Parameters and domain

0<=line_width_i<=cell_size_i, cell_size_i>0; lo/hi default 0/1; canvas/dtype.

## Mathematics

q_i=(p_i-offset_i)-cell_i*floor((p_i-offset_i)/cell_i). Select hi if either q_i<width_i, else lo. Lines touch the left/top period boundary; crossings are not additive.

## Behavior

Support class: Regional.

## Acceptance

Zero width all lo; overlapping grid lines; fractional cells and negative coordinates.
