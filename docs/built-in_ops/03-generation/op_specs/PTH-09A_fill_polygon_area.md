---
spec_schema_version: 1
id: PTH-09A
parent_id: PTH-09
function: fill_polygon_area
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.fill_polygon_area_v1_strict
  - path.fill_polygon_area_v1_accelerated_apple_silicon
  - path.fill_polygon_area_v1_accelerated_x86_64
---

# PTH-09A: fill polygon area

Inherits [PTH-09](PTH-09_fill_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.fill_polygon_area_v1_strict`, `path.fill_polygon_area_v1_accelerated_apple_silicon`, `path.fill_polygon_area_v1_accelerated_x86_64`.

M/L/Z PathSet; raw scalar coverage[H,W] Float32/64.

### Parameters and domain

canvas/dtype; fill_rule nonzero default or evenodd; self-crossing, holes and overlaps permitted.

## Mathematics

RN_T(area(pixel intersect regularized_fill(path,rule))). Inputs are exact dyadic coordinates, intersections/areas rational. Fill open contours with an implicit consumption-only closing edge. Set coverage, not contour alpha-over; zero-area edges contribute nothing. Final[0,1] without error-hiding clamp.

## Behavior

Support class: Regional. Each pixel depends on the complete path; the path payload is not validated.

## Acceptance

Half pixel, unit square, bow-tie1/2, duplicate contours nonzero1/evenodd0, holes/shared edges/tiny rational areas.
