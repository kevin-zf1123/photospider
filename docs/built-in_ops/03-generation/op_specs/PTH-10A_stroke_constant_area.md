---
spec_schema_version: 1
id: PTH-10A
parent_id: PTH-10
function: stroke_constant_area
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.stroke_constant_area_v1_strict
  - path.stroke_constant_area_v1_accelerated_apple_silicon
  - path.stroke_constant_area_v1_accelerated_x86_64
---

# PTH-10A: stroke constant area

Inherits [PTH-10](PTH-10_stroke_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.stroke_constant_area_v1_strict`, `path.stroke_constant_area_v1_accelerated_apple_silicon`, `path.stroke_constant_area_v1_accelerated_x86_64`.

Polyline PathSet,constant width[1]>=0 in px; coverage[H,W].

### Parameters and domain

canvas/dtype; cap=round default/butt/square, join=round default/bevel/miter; miter_limit>=1,constructor4; closed paths ignore caps.

## Mathematics

Radius r=width/2 exactly. Each nonzero segment contributes the rectangle between its normal offsets +/-r. At an open endpoint round adds a semicircle, butt adds nothing, and square extends the rectangle by r along the endpoint tangent. Inner joins are covered by the union of strips. Outer round joins add the circular sector; bevel joins add the triangle connecting outer strip corners; miter joins extend the outer side lines to their intersection. Union these regions before pixel-area RN. Miter compares exact outer distance/radius to limit, equality keeps miter, excess bevels. At a 180-degree reversal round adds a semicircle; bevel/miter do not produce an infinite spike. Zero width empty; isolated point round=disk,square=axis-aligned width square,butt=empty.

## Behavior

Support class: Regional. Each pixel depends on the complete stroked path. Overlapping strips are unioned, not accumulated per component.

## Acceptance

Butt rectangle, point disk/square, acute miter threshold, overlap not darkened, subpixel coverage.
