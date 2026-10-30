---
spec_schema_version: 1
id: PTH-10C
parent_id: PTH-10
function: stroke_outline_flatten
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.stroke_outline_flatten_v1_strict
---

# PTH-10C: stroke outline flatten

Inherits [PTH-10](PTH-10_stroke_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.stroke_outline_flatten_v1_strict`.

Explicit constant or variable-round stroke recipe; closed CoreVerbs outline and GeometryReport.

### Parameters and domain

epsilon_outline_px>0 default0.05; max_segments,attribute_policy; topology allow_change default or reject_unproven.

`width_mode=constant` consumes width `[1]`; `width_mode=variable_round` consumes widths `[C]`. The topology parameter is `topology_policy`, default `allow_change`. Defaults: `max_depth=52`, `max_segments=1048576`, `max_refinements=4096`. `topology_policy=reject_unproven` requires zero approximation and publication bounds (geometric identity), otherwise `InvalidQuality`.

## Mathematics

Construct the exact exposed stroke boundary then deterministic equal-angle DFS subdivision of arcs, bounding chord sagitta plus RN64 endpoint displacement. Straight edges preserved, contours canonicalized. Default reports unverified topology; strict policy proves required feature/face relations or fails. Output polygon area is not original curved-stroke area.

### v1 error accounting

Independently round every exact arc sagitta and endpoint displacement upward to Float64, retain each stage's maximum, then compare the exact rational sum of these published upper bounds with `epsilon_outline_px`. The approximation and publication stages are reported separately. Subdivision is deterministic equal-angle, left-first. The publication maximum is global over the vertices of all final exposed contours, including straight-edge endpoints; if publication alone exceeds the requested bound the call fails with `InvalidQuality`. Newly retained split vertices can increase publication error, so earlier leaves are rechecked and refined again in left-first order until the global sum fits. Initial edges and added splits both count against `max_segments`; no prefix is published on failure, and exhausting any hard limit fails the whole call.

## Behavior

Support class: complete input. No width is modified implicitly.

## Acceptance

Butt line rectangle, circle outline bound, narrow-hole strict failure and permitted geometric difference from direct exact coverage. The diagonal `(0,0)->(1,1)`, width 1, butt cap fails at `epsilon_outline_px=1e-20` and succeeds at `5e-17`. Include multiple translated round contours whose maximum sagitta and maximum publication displacement occur at different vertices; require the exact sum of the reported stage bounds to satisfy the request. Check initial-edge, split and depth limits.
