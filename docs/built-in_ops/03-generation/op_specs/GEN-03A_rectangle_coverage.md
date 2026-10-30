---
spec_schema_version: 1
id: GEN-03A
parent_id: GEN-03
function: rectangle_coverage
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.rectangle_coverage_v1_strict
  - generation.rectangle_coverage_v1_accelerated_apple_silicon
  - generation.rectangle_coverage_v1_accelerated_x86_64
---

# GEN-03A: rectangle coverage

Inherits [GEN-03](GEN-03_basic_shapes_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.rectangle_coverage_v1_strict`, `generation.rectangle_coverage_v1_accelerated_apple_silicon`, `generation.rectangle_coverage_v1_accelerated_x86_64`.

bounds[4]=(xmin,ymin,xmax,ymax); coverage[H,W].

### Parameters and domain

canvas, dtype default Float64; finite ordered bounds xmin<=xmax and ymin<=ymax are unchecked caller premises. Equality gives empty area.

## Mathematics

At pixel lower corner (x,y), RN_T(max(0,min(x+1,xmax)-max(x,xmin))*max(0,min(y+1,ymax)-max(y,ymin))). Round the full rational product once.

## Behavior

Support class: Regional.

## Acceptance

Bounds (1/4,1/4,3/4,3/4) cover 1/4 of the unit pixel; equal bounds give zero coverage; integer translation preserves coverage. Reversed bounds are outside the production input domain.
