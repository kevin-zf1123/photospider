---
spec_schema_version: 1
id: PTH-06B
parent_id: PTH-06
function: transform_stroke_outline
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# PTH-06B: transform stroke outline

Inherits [PTH-06](PTH-06_transforms_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Closed stroke outline PathSet and affine[2,3]; transformed closed outline.

### Parameters and domain

Nonsingular affine only; no centerline-width propagation.

Public SDK helper, declared in `photospider/ops/generation.hpp`:

```cpp
struct PathWithReport final { WorkflowNodeOutput path, report; };
Result<PathWithReport> ps::generation::transform_stroke_outline(
    WorkflowDocument* document, WorkflowInput outline, WorkflowInput affine,
    WorkflowInput prior_report);
```

The helper composes PTH-06A with `path.transform_outline_report_v1_strict`. The prior GeometryReport is an explicit input. The report scales the prior approximation, quantization and publication bounds by the exact spectral norm of the affine and adds the coordinate publication error of the new transform. Filling the transformed closed outline is a separate PTH-09 call.

## Mathematics

Transform the outline geometry, then fill explicitly using PTH-09. A round cap becomes elliptical under nonuniform scaling; do not scale a raster mask instead. Propagate prior polygonization error by at most spectral norm(A) and account subsequent publication error.

## Behavior

Support class: complete input. The downstream fill is Regional.

## Acceptance

Width2 horizontal outline scaled3 in y has height6, unlike centerline-transform then width2 stroke; reflected orientation.
