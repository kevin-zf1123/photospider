---
spec_schema_version: 1
id: GEN-03B
parent_id: GEN-03
function: ellipse_coverage
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.ellipse_coverage_v1_strict
  - generation.ellipse_coverage_v1_accelerated_apple_silicon
  - generation.ellipse_coverage_v1_accelerated_x86_64
---

# GEN-03B: ellipse coverage

Inherits [GEN-03](GEN-03_basic_shapes_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.ellipse_coverage_v1_strict`, `generation.ellipse_coverage_v1_accelerated_apple_silicon`, `generation.ellipse_coverage_v1_accelerated_x86_64`.

center[2], positive radii[2]; coverage[H,W].

### Parameters and domain

canvas, dtype default Float64, Int64 max_refinements default 4096; axis-aligned only.

`max_refinements` is the full positive Int64 refinement limit; it is not reduced modulo 2^32. Limits 4096, 2^32, 2^32+1 and INT64_MAX produce identical bits whenever the same rounding certificate is reached within 4096 refinements.

## Mathematics

The object is ((X-cx)/rx)^2+((Y-cy)/ry)^2<=1. Return RN_T of its exact intersection area with the unit pixel. Exact full/empty classification gives 1/0; partial intersections need certified integration.

## Behavior

Support class: Regional.

## Acceptance

Quarter-unit-circle area pi/4, full area pi*rx*ry, tangent contact zero, thin ellipses. Measured integration alone does not certify strict output.
