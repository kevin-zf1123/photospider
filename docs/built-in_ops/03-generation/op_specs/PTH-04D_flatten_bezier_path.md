---
spec_schema_version: 1
id: PTH-04D
parent_id: PTH-04
function: flatten_bezier_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.flatten_bezier_path_v1_strict
---

# PTH-04D: flatten bezier path

Inherits [PTH-04](PTH-04_resampling_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.flatten_bezier_path_v1_strict`.

Core/Bezier PathSet; Polyline PathSet and optional associated parameter samples.

### Parameters and domain

epsilon_geom_px>0 default0.05, explicit preview0.25; max_depth24,max_segments; attribute_policy; topology allow_change default or reject_unproven.

Outputs: `path`, `report`, `samples` and `source_map`. The exact dyadic subdivision parameters are encoded in `path_source_map`. `topology=reject_unproven` accepts only the exact geometric-identity certificate; a nonzero bound returns `InvalidQuality`. This is a conservative success domain, not a general topology solver.

## Mathematics

Exact t=1/2 de Casteljau, DFS left first. Bound parameter-matched curve/chord deviation by differences between degree-d controls and degree-elevated endpoint chord controls. The parameter-matched bound includes the maximum RN64 endpoint displacement and is checked on each accepted leaf. Shared vertices once, closed Z without duplicate M. Never use perpendicular flatness alone.

## Behavior

Support class: complete input. A strict topology request needs proof or fails.

## Acceptance

Straight line, collinear reversal, certified total error including publication; sampled checks supplement the control bound, not replace it.
