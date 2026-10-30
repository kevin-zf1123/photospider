---
spec_schema_version: 1
id: PTH-09B
parent_id: PTH-09
function: fill_bezier_flatten
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# PTH-09B: fill bezier flatten

Inherits [PTH-09](PTH-09_fill_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Core/Bezier PathSet; coverage[H,W] and optional GeometryReport.

### Parameters and domain

canvas/dtype/fill_rule; epsilon_geom_px default0.05 and flatten limits (max_depth, max_segments); topology allow_change default or reject_unproven.

Public SDK helper, declared in `photospider/ops/generation.hpp`:

```cpp
struct PathRasterization final { WorkflowNodeOutput coverage, report; };
Result<PathRasterization> ps::generation::fill_bezier_path(
    WorkflowDocument* document, WorkflowInput path,
    Parameters flatten_parameters, Parameters fill_parameters,
    Profile fill_profile = Profile::Strict);
```

The helper composes `path.flatten_bezier_path_v1_strict` (PTH-04D) and `path.fill_polygon_area_v1_<profile>` (PTH-09A) and returns the fill `coverage` and the flatten GeometryReport as separate outputs. The report refers to the source curve and is a geometry-distance certificate, not an area-error certificate. There is no PTH-09B operation key.

## Mathematics

Publish PTH-04D's deterministic polyline, then apply PTH-09A. Geometry approximation and final area RN are distinct stages. Strict topology needs proof; default reports unverified unless a change is established. Do not claim source-curve area error<=epsilon_geom_px.

## Behavior

Support class: Regional. The coverage depends on the complete flattened polyline.

## Acceptance

Line equivalence, epsilon-dependent geometry, near-contact topology and actual ROI computation.
