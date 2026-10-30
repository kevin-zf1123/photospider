---
spec_schema_version: 1
id: GEN-04A
parent_id: GEN-04
function: checker_pattern
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.checker_pattern_v1_strict
---

# GEN-04A: checker pattern

Inherits [GEN-04](GEN-04_test_patterns_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.checker_pattern_v1_strict`.

cell_size[2], offset[2]; values[H,W].

### Parameters and domain

Positive cell size in px; canvas, dtype default Float64; finite lo/hi default 0/1.

## Mathematics

i=floor((px-offsetx)/cellx), j=floor((py-offsety)/celly). Select lo for even i+j, hi otherwise; exact floor for negative coordinates, selected value converted under copy rules.

## Behavior

Support class: Regional.

## Acceptance

Unit checkerboard, one-pixel phase flip, negative-coordinate floor.
